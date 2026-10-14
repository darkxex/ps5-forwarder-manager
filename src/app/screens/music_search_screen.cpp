// ps5fwdgen - Search YouTube for a game's theme music, and listen first.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Laid out after ps5-homebrew-ui's "Now Playing" concept (src/concepts/
// player.cpp): large artwork, the track, a spectrum visualizer, a scrubber, a
// transport row and a frosted queue, here holding the search results.
// Unlike the concept it really plays: a track streams from YouTube as it
// downloads (AAC decoded while the rest arrives) and the visualizer reads the
// audio that is audible, so the whole screen listens to the same music.
//   - the artwork is the video's thumbnail and the play state: it rests at
//     94 % and dims when paused, and on a track change the cover and the text
//     slide through in the direction of travel;
//   - the backdrop and every accent take the palette of the thumbnail;
//   - the scrubber covers the clip that would become the selection music (at
//     most 85 s), with what has downloaded so far drawn under it;
//   - one focus ring glides between the transport buttons, locks onto the
//     scrubber's thumb and stretches onto "Use this track"; in the queue a
//     highlight springs between the rows;
//   - "Use this track" reuses the bytes already downloaded for listening, has
//     them converted to ATRAC9 and hands the clip to the forwarder.
// L1 / R1 step through the searches SDH-GameThemeMusic-style ("<game> Theme
// Music", then menu, title screen and soundtrack); Triangle searches again.
//
// Network and decoding run on worker threads; the main thread hands decoded
// PCM to the mixer's preview deck each frame and uploads GL textures (the only
// place the GL context is current).

#include "app/keyboard_prompt.hpp"
#include "app/screens/music_search_screen.hpp"

#include "app/context.hpp"
#include "app/preview_music.hpp"
#include "audio/at9.hpp"
#include "audio/mp4_aac.hpp"
#include "core/tween.hpp"
#include "fwd/image.hpp"
#include "gfx/renderer.hpp"
#include "net/at9_convert.hpp"
#include "net/steamgriddb.hpp"
#include "net/youtube.hpp"
#include "platform/ps5/system.hpp"
#include "ui/components/input_prompt.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fwd
{

namespace
{

extern "C" void glDeleteTextures(int n, const unsigned int *textures);

using hui::gfx::Color;
using hui::gfx::Rect;
namespace tween = hui::tween;

// ---- the design language (Now Playing) ---------------------------------------
const Color kWhite = Color::rgb(0xffffff);
const Color kInk = Color::rgb(0x0b0d16); // icons on the white play button
const Color kClear = Color::rgb(0x000000, 0.0f);
const Color kWarn = Color::rgb(0xffb4a8);

constexpr float kWidth = hui::gfx::kVirtualWidth;
constexpr float kMargin = 96.0f;
constexpr float kArt = 520.0f;
constexpr float kArtY = 172.0f;
constexpr float kArtRadius = 40.0f;
constexpr float kPausedScale = 0.94f;
constexpr float kColumnGap = 84.0f;
constexpr float kWideMargin = 218.0f; // side margin when the queue is hidden

constexpr int kBars = 32;
constexpr float kBarsBase = 640.0f;
constexpr float kBarsHeight = 204.0f;

constexpr float kScrubY = 772.0f;
constexpr float kTransportY = 888.0f;
constexpr float kPlayRadius = 46.0f;
constexpr float kButtonRadius = 32.0f;
constexpr float kUseW = 290.0f; // the "Use this track" pill
constexpr float kUseH = 64.0f;

constexpr float kQueueX = 1404.0f;
constexpr float kQueueY = 96.0f;
constexpr float kQueueW = 420.0f;
constexpr float kQueueH = 856.0f;
constexpr float kQueueRadius = 36.0f;
constexpr float kQueueListTop = 104.0f;
constexpr float kQueueRow = 66.0f;

constexpr float kSeekStep = 5.0f;
constexpr float kSeekJump = 15.0f;
constexpr float kRestartAfter = 3.0f;
constexpr float kSoft = 10.0f;

// At most this many results are shown (and their thumbnails fetched).
constexpr std::size_t kMaxResults = 16;

// The searches: what is appended to the game's name.
struct Variant
{
    const char *label;
    const char *suffix;
};
constexpr Variant kVariants[] = {
    {"Theme music", yt::kDefaultSuffix}, // SDH-GameThemeMusic's own search
    {"Main menu", " Main Menu Music"},
    {"Title screen", " Title Screen Music"},
    {"Soundtrack", " OST"},
};
constexpr int kVariantCount = static_cast<int>(sizeof(kVariants) / sizeof(kVariants[0]));

enum class Zone
{
    scrubber,
    transport,
    queue,
};

enum Control
{
    kPrevious,
    kPlay,
    kNext,
    kUse,
    kControls,
};
constexpr float kControlOffset[3] = {-114.0f, 0.0f, 114.0f};

// ---- text ----------------------------------------------------------------------

// A file name's region and dump tags ("(USA)", "[!]") only get in the way
// of a search: "Castlevania - Symphony of the Night (USA)" -> "Castlevania -
// Symphony of the Night".
std::string search_name(const std::string &name)
{
    std::string out;
    int depth = 0;
    for (const char c : name)
    {
        if (c == '(' || c == '[')
            ++depth;
        else if ((c == ')' || c == ']') && depth > 0)
            --depth;
        else if (depth == 0 && !(c == ' ' && (out.empty() || out.back() == ' ')))
            out.push_back(c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '-'))
        out.pop_back();
    return out.empty() ? name : out;
}

// Text for the screen's fonts: a character they lack becomes its plain
// look-alike ("Vidéo" -> "Video", curly quotes -> straight ones, dashes ->
// '-') instead of the font's "?".
std::string printable(const hui::gfx::Font &font, std::string_view text)
{
    static const char kLatin1[] =
        "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    std::string out;
    std::size_t i = 0;
    while (i < text.size())
    {
        const auto c = static_cast<unsigned char>(text[i]);
        const std::size_t len = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        if (len == 1 || i + len > text.size())
        {
            out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
            ++i;
            continue;
        }
        std::uint32_t cp = c & (0xff >> (len + 1));
        for (std::size_t k = 1; k < len; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3f);
        if (font.has_glyph(cp))
            out.append(text.substr(i, len));
        else if (cp >= 0xc0 && cp <= 0xff)
            out.push_back(kLatin1[cp - 0xc0]);
        else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201a || cp == 0x2032)
            out.push_back('\'');
        else if (cp == 0x201c || cp == 0x201d || cp == 0x201e || cp == 0x2033)
            out.push_back('"');
        else if (cp >= 0x2010 && cp <= 0x2015)
            out.push_back('-');
        else if (cp == 0x2026)
            out += "...";
        else if (cp == 0xa0 || cp == 0x3000)
            out.push_back(' ');
        else
            out.append(text.substr(i, len));
        i += len;
    }
    return out;
}

// JPEG or PNG bytes, not a bot-check page.
bool looks_like_image(const std::vector<unsigned char> &bytes)
{
    return bytes.size() > 8 && ((bytes[0] == 0xff && bytes[1] == 0xd8) ||
                                (bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N'));
}

std::string clock_text(double seconds)
{
    const long whole = std::max(0L, static_cast<long>(seconds));
    char text[32];
    if (whole >= 3600)
        (void)std::snprintf(text, sizeof(text), "%ld:%02ld:%02ld", whole / 3600, (whole / 60) % 60,
                            whole % 60);
    else
        (void)std::snprintf(text, sizeof(text), "%ld:%02ld", whole / 60, whole % 60);
    return text;
}

// A triangle pointing left (dir -1) or right (dir 1), drawn as three capsule
// lines: smooth edges, soft corners (as the concept draws it).
void side_triangle(hui::gfx::DrawList &list, float cx, float cy, float size, float dir, Color color)
{
    const float stroke = size * 0.4f;
    const float half = size * 0.3f;
    const float back = cx - dir * size * 0.22f;
    const float tip = cx + dir * size * 0.3f;
    list.line(back, cy - half, back, cy + half, stroke, color);
    list.line(back, cy - half, tip, cy, stroke, color);
    list.line(back, cy + half, tip, cy, stroke, color);
}

// ---- background work -----------------------------------------------------------

// Runs fn on its own detached thread; it owns what it captures, so a screen
// that closes never waits for a slow network call.
template <typename Fn> bool run_detached(Fn fn)
{
    auto *task = new Fn(std::move(fn));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int rc = pthread_create(
        &thread, &attr,
        [](void *arg) -> void *
        {
            std::unique_ptr<Fn> own(static_cast<Fn *>(arg));
            (*own)();
            return nullptr;
        },
        task);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        delete task;
    return rc == 0;
}

// The search and its thumbnails.
struct SearchJob
{
    std::string term;
    std::atomic<bool> done{false};
    std::atomic<int> progress{0};
    std::atomic<int> total{0};
    std::mutex mutex;
    bool ok = false;
    std::string error;
    std::vector<yt::Video> videos;
    std::vector<std::vector<unsigned char>> thumbs; // index-aligned to videos
};

// One track streaming: its download, decoded as it arrives.
struct StreamJob
{
    std::string id;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::atomic<int> got{0};   // bytes downloaded
    std::atomic<int> total{0}; // ... out of
    std::mutex mutex;
    bool ok = false;
    std::string error;
    std::vector<std::int16_t> pending; // decoded, not yet handed to the mixer
    int rate = 0;
    int channels = 0;
    std::vector<unsigned char> file; // the whole download, once done
    std::string filename;
};

// The chosen track becoming ATRAC9.
struct ConvertJob
{
    std::atomic<bool> done{false};
    std::mutex mutex;
    bool ok = false;
    std::string error;
    std::vector<unsigned char> at9;
};

// ---- one search result -----------------------------------------------------------

struct Result
{
    yt::Video video;
    std::string title;  // printable
    std::string author; // printable
    std::uint32_t texture = 0;
    Rect uv{0.0f, 0.0f, 1.0f, 1.0f}; // the square of the thumbnail shown
    Color dark = Color::rgb(0x1a1030);
    Color mid = Color::rgb(0x5b3f8f);
    Color accent = Color::rgb(0xff8fb1);
};

// The thumbnail's square: the picture without the black bars YouTube pads
// it with (letterbox or pillarbox, of any size), cropped to its centre.
Rect square_uv(int w, int h, const std::vector<unsigned char> &rgba)
{
    // A row or column is a bar when it is all near-black.
    const auto dark = [&](int x0, int y0, int dx, int dy, int count)
    {
        for (int i = 0; i < count; i += 3)
        {
            const std::size_t at =
                (static_cast<std::size_t>(y0 + dy * i) * static_cast<std::size_t>(w) +
                 static_cast<std::size_t>(x0 + dx * i)) *
                4;
            if (rgba[at] > 24 || rgba[at + 1] > 24 || rgba[at + 2] > 24)
                return false;
        }
        return true;
    };
    int top = 0;
    int bottom = h;
    int left = 0;
    int right = w;
    while (top < h / 3 && dark(0, top, 1, 0, w))
        ++top;
    while (bottom > h * 2 / 3 && dark(0, bottom - 1, 1, 0, w))
        --bottom;
    while (left < w / 3 && dark(left, top, 0, 1, bottom - top))
        ++left;
    while (right > w * 2 / 3 && dark(right - 1, top, 0, 1, bottom - top))
        --right;
    const float cw = static_cast<float>(right - left);
    const float ch = static_cast<float>(bottom - top);
    const float side = std::min(cw, ch);
    const float x = static_cast<float>(left) + (cw - side) * 0.5f;
    const float y = static_cast<float>(top) + (ch - side) * 0.5f;
    return {x / static_cast<float>(w), y / static_cast<float>(h), side / static_cast<float>(w),
            side / static_cast<float>(h)};
}

// Three tones from the picture: its mean for the body, a darker one for the
// backdrop and a brighter, saturated one for accents.
void palette_from(const std::vector<unsigned char> &rgba, Result &result)
{
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4 * 7)
    {
        r += rgba[i];
        g += rgba[i + 1];
        b += rgba[i + 2];
        ++n;
    }
    if (n == 0)
        return;
    Color mean{static_cast<float>(r / n / 255.0), static_cast<float>(g / n / 255.0),
               static_cast<float>(b / n / 255.0), 1.0f};
    const float mx = std::max({mean.r, mean.g, mean.b, 0.01f});
    const float mn = std::min({mean.r, mean.g, mean.b});
    if (mx - mn < 0.06f) // a grey picture: keep the default violet
        return;
    // Stretch the saturation so the colour reads, then set the brightness.
    const auto tone = [&](float stretch, float level)
    {
        Color c{(mean.r - mn) * stretch + mn, (mean.g - mn) * stretch + mn,
                (mean.b - mn) * stretch + mn, 1.0f};
        const float top = std::max({c.r, c.g, c.b, 0.01f});
        return Color{std::min(1.0f, c.r / top * level), std::min(1.0f, c.g / top * level),
                     std::min(1.0f, c.b / top * level), 1.0f};
    };
    result.mid = tone(1.6f, 0.55f);
    result.dark = hui::gfx::mix(tone(1.4f, 0.32f), Color::rgb(0x05060c), 0.35f);
    result.accent = hui::gfx::mix(tone(2.2f, 1.0f), kWhite, 0.15f);
}

// ---- the screen ------------------------------------------------------------------

class MusicSearchScreen final : public Screen
{
  public:
    MusicSearchScreen(Context &context, std::string query,
                      std::function<void(std::vector<unsigned char>)> on_at9)
        : query_(search_name(query)), on_at9_(std::move(on_at9))
    {
        restyle(context);
        bind_keyboard_prompt(prompt_); // R2 confirms
        prompt_.style.buttons = false;
        prompt_.set_title("Search theme music (game name)");
        prompt_.style.max_length = 80;
        apply_palette(true);
        play_.snap(0.0f);
        morph_.snap(0.0f);
        queue_.snap(1.0f);
        ring_.snap(ring_target());
        levels_.fill(0.0f);
        peaks_.fill(0.0f);
    }

    ~MusicSearchScreen() override
    {
        // Jobs still running finish on their own threads; stop the downloads.
        if (stream_)
            stream_->cancel.store(true);
        release_textures();
    }

    void restyle(Context &context) override
    {
        spinner_.style.theme = context.theme;
        spinner_.style.kind = hui::ui::SpinnerKind::arc;
        spinner_.style.color = kWhite;
        style_keyboard_prompt(prompt_, context.theme);
    }

    void enter(Context &context) override
    {
        (void)context;
        if (!started_)
        {
            started_ = true;
            hui::ui::Feedback discard;
            prompt_.open(discard, query_);
        }
    }

    void backdrop(Context &context, hui::gfx::BackdropSpec &spec) const override
    {
        (void)context;
        spec.mode = hui::gfx::BackdropMode::bokeh;
        for (int i = 0; i < 4; ++i)
            spec.colors[i] = palette_[i].value();
        spec.time = drift_;
    }

    // ---- searching ---------------------------------------------------------------
    void start_search()
    {
        search_term_ = query_ + kVariants[variant_].suffix;
        auto job = std::make_shared<SearchJob>();
        job->term = search_term_;
        search_ = job;
        searching_ = true;
        message_.clear();
        run_detached(
            [job]()
            {
                std::vector<yt::Video> videos;
                const yt::Result result = yt::search(job->term, videos);
                if (videos.size() > kMaxResults)
                    videos.resize(kMaxResults);
                hui::sys::log("[FWD] music search ok=%d results=%zu err=%s", result.ok ? 1 : 0,
                              videos.size(), result.error.c_str());
                // Thumbnails: each from the first host that gives a picture; a
                // host that fails once is not asked again.
                std::vector<std::vector<unsigned char>> thumbs(videos.size());
                std::vector<std::string> dead;
                job->total.store(static_cast<int>(videos.size()));
                for (std::size_t i = 0; i < videos.size(); ++i)
                {
                    for (const std::string &url : videos[i].thumbs)
                    {
                        const std::string host = url.substr(0, url.find('/', 8));
                        if (std::find(dead.begin(), dead.end(), host) != dead.end())
                            continue;
                        std::vector<unsigned char> bytes;
                        if (sgdb::download(url, bytes).ok && looks_like_image(bytes))
                        {
                            thumbs[i] = std::move(bytes);
                            break;
                        }
                        dead.push_back(host);
                    }
                    job->progress.store(static_cast<int>(i + 1));
                }
                std::lock_guard<std::mutex> lock(job->mutex);
                job->ok = result.ok;
                job->error = result.error;
                job->videos = std::move(videos);
                job->thumbs = std::move(thumbs);
                job->done.store(true);
            });
    }

    void poll_search(Context &context)
    {
        if (!search_ || !search_->done.load())
            return;
        const std::shared_ptr<SearchJob> job = std::move(search_);
        searching_ = false;
        std::lock_guard<std::mutex> lock(job->mutex);
        if (!job->ok)
        {
            message_ = job->error.empty() ? "The search failed" : job->error;
            return;
        }
        stop_playback(context);
        release_textures();
        results_.clear();
        for (std::size_t i = 0; i < job->videos.size(); ++i)
        {
            Result result;
            result.video = job->videos[i];
            result.title = printable(*context.fonts.semibold.font, result.video.title);
            result.author = printable(*context.fonts.regular.font, result.video.author);
            if (i < job->thumbs.size() && !job->thumbs[i].empty())
            {
                int w = 0;
                int h = 0;
                std::vector<unsigned char> rgba;
                if (fwd::decode_image(job->thumbs[i].data(), job->thumbs[i].size(), w, h, rgba) &&
                    w > 0 && h > 0)
                {
                    result.texture = context.renderer.batch().create_texture(w, h, rgba.data());
                    result.uv = square_uv(w, h, rgba);
                    palette_from(rgba, result);
                }
            }
            results_.push_back(std::move(result));
        }
        hui::sys::log("[FWD] music results=%zu", results_.size());
        index_ = 0;
        previous_ = 0;
        queue_row_index_ = 0;
        queue_row_.snap(0.0f);
        queue_scroll_.reveal(0.0f, kQueueRow, queue_view_height(), kQueueRow);
        if (results_.empty())
        {
            message_ = "No videos for that search; try L1 / R1 for another.";
            return;
        }
        apply_palette(false);
        swap_.start(context.settings.reduced_motion ? 0.14f : 0.6f);
        // Like SDH-GameThemeMusic, the first result starts playing at once.
        start_stream(context);
    }

    void release_textures()
    {
        std::vector<std::uint32_t> textures;
        for (const Result &result : results_)
            if (result.texture != 0)
                textures.push_back(result.texture);
        if (!textures.empty())
            glDeleteTextures(static_cast<int>(textures.size()), textures.data());
        for (Result &result : results_)
            result.texture = 0;
    }

    // ---- streaming -----------------------------------------------------------------
    // The clip that would become the selection music: at most 85 s.
    double clip_seconds() const
    {
        if (results_.empty())
            return at9::kMaxMusicSeconds;
        const long seconds = results_[static_cast<std::size_t>(index_)].video.seconds;
        return seconds > 0 ? std::min<double>(seconds, at9::kMaxMusicSeconds)
                           : at9::kMaxMusicSeconds;
    }

    void start_stream(Context &context)
    {
        if (results_.empty() || context.music == nullptr)
            return;
        if (stream_)
            stream_->cancel.store(true);
        auto job = std::make_shared<StreamJob>();
        job->id = results_[static_cast<std::size_t>(index_)].video.id;
        stream_ = job;
        fed_frames_ = 0;
        stream_ended_ = false;
        context.music->stream_start();
        playing_ = true;
        hui::sys::log("[FWD] music stream id=%s", job->id.c_str());
        run_detached(
            [job]()
            {
                const std::int64_t started = hui::sys::monotonic_us();
                aac::Mp4Stream decoder;
                std::vector<std::int16_t> pcm;
                std::vector<unsigned char> file;
                std::string filename;
                const yt::Result result = yt::download_audio(
                    job->id, at9::kMaxMusicSeconds, net::kAt9UploadLimit, file, filename,
                    [&job](std::size_t done, std::size_t total)
                    {
                        job->got.store(static_cast<int>(done));
                        job->total.store(static_cast<int>(total));
                    },
                    [&](const unsigned char *data, std::size_t size)
                    {
                        if (!decoder.feed(data, size, pcm))
                        {
                            job->cancel.store(true); // not playable: stop downloading
                            return;
                        }
                        std::lock_guard<std::mutex> lock(job->mutex);
                        job->pending.insert(job->pending.end(), pcm.begin(), pcm.end());
                        job->rate = decoder.sample_rate();
                        job->channels = decoder.channels();
                        pcm.clear();
                    },
                    &job->cancel);
                std::lock_guard<std::mutex> lock(job->mutex);
                job->ok = result.ok && decoder.error().empty();
                job->error = !decoder.error().empty()
                                 ? "this video's audio cannot be played (" + decoder.error() + ")"
                                 : result.error;
                if (job->ok)
                {
                    job->file = std::move(file);
                    job->filename = std::move(filename);
                }
                hui::sys::log("[FWD] music stream id=%s ok=%d bytes=%zu ms=%lld err=%s",
                              job->id.c_str(), job->ok ? 1 : 0, job->file.size(),
                              static_cast<long long>((hui::sys::monotonic_us() - started) / 1000),
                              job->error.c_str());
                job->done.store(true);
            });
    }

    // Hands what the stream decoded since the last frame to the mixer, up to
    // the end of the clip.
    void pump_stream(Context &context)
    {
        if (!stream_ || context.music == nullptr || stream_ended_)
            return;
        std::vector<std::int16_t> pcm;
        int rate = 0;
        int channels = 0;
        bool done = false;
        bool ok = false;
        std::string error;
        {
            std::lock_guard<std::mutex> lock(stream_->mutex);
            pcm.swap(stream_->pending);
            rate = stream_->rate;
            channels = stream_->channels;
            done = stream_->done.load();
            ok = stream_->ok;
            error = stream_->error;
        }
        if (rate > 0 && channels > 0 && !pcm.empty())
        {
            const auto clip = static_cast<std::size_t>(clip_seconds() * rate);
            std::size_t frames = pcm.size() / static_cast<std::size_t>(channels);
            if (fed_frames_ + frames > clip)
                frames = clip > fed_frames_ ? clip - fed_frames_ : 0;
            if (frames > 0)
                context.music->stream_append(pcm.data(), frames, channels, rate);
            fed_frames_ += frames;
            if (fed_frames_ >= clip)
            {
                context.music->stream_end();
                stream_ended_ = true;
            }
        }
        if (done && !stream_ended_)
        {
            context.music->stream_end();
            stream_ended_ = true;
            if (!ok && error != "cancelled")
                message_ = error;
        }
    }

    void stop_playback(Context &context)
    {
        if (stream_)
            stream_->cancel.store(true);
        stream_.reset();
        playing_ = false;
        if (context.music != nullptr)
            context.music->stop();
    }

    double position(Context &context) const
    {
        return context.music != nullptr && context.music->streaming() ? context.music->position()
                                                                      : 0.0;
    }

    // ---- "Use this track" -----------------------------------------------------------
    void request_use(Context &context, hui::ui::Feedback &feedback)
    {
        if (results_.empty() || converting_)
        {
            refuse(feedback, 1.0f);
            return;
        }
        feedback.play(hui::audio::Cue::select, 1.0f, 0.4f);
        message_.clear();
        const std::string &id = results_[static_cast<std::size_t>(index_)].video.id;
        if (!stream_ || stream_->id != id || (stream_->done.load() && !stream_->ok))
            start_stream(context); // not listened to yet (or it failed): fetch it now
        use_pending_ = true;
    }

    // Once the chosen track's download is complete, convert it.
    void poll_use(Context &context)
    {
        if (convert_ && convert_->done.load())
        {
            const std::shared_ptr<ConvertJob> job = std::move(convert_);
            converting_ = false;
            std::lock_guard<std::mutex> lock(job->mutex);
            if (job->ok)
            {
                if (context.music != nullptr)
                    context.music->stop(); // the edit screen plays the new clip
                if (on_at9_)
                    on_at9_(std::move(job->at9));
                finished_ = true;
            }
            else
            {
                message_ = job->error;
            }
            return;
        }
        if (!use_pending_ || !stream_ || !stream_->done.load())
            return;
        use_pending_ = false;
        std::vector<unsigned char> file;
        std::string filename;
        {
            std::lock_guard<std::mutex> lock(stream_->mutex);
            if (!stream_->ok)
            {
                message_ = stream_->error;
                return;
            }
            file = stream_->file; // kept: the track can be used again after a failure
            filename = stream_->filename;
        }
        auto job = std::make_shared<ConvertJob>();
        convert_ = job;
        converting_ = true;
        run_detached(
            [job, file = std::move(file), filename = std::move(filename)]()
            {
                const std::int64_t started = hui::sys::monotonic_us();
                std::vector<unsigned char> at9;
                std::string error;
                const bool converted = net::convert_to_at9(file, filename, at9, error);
                if (converted)
                    at9 = at9::trim(at9, at9::kMaxMusicSeconds); // the format's limit
                hui::sys::log("[FWD] music convert ok=%d at9=%zu seconds=%.1f ms=%lld err=%s",
                              converted ? 1 : 0, at9.size(), at9::duration(at9.data(), at9.size()),
                              static_cast<long long>((hui::sys::monotonic_us() - started) / 1000),
                              error.c_str());
                std::lock_guard<std::mutex> lock(job->mutex);
                job->ok = converted;
                job->error = converted ? std::string() : "could not convert the audio: " + error;
                job->at9 = std::move(at9);
                job->done.store(true);
            });
    }

    // ---- layout ----------------------------------------------------------------------
    struct Layout
    {
        float x = 0.0f;
        float w = 0.0f;
        float cx = 0.0f;
        Rect art;
        float column_x = 0.0f;
        float column_w = 0.0f;
    };

    Layout layout() const
    {
        const float q = queue_.value;
        Layout l;
        l.x = tween::lerp(kWideMargin, kMargin, q);
        l.w = tween::lerp(kWidth - 2.0f * kWideMargin, kQueueX - 64.0f - kMargin, q);
        l.cx = l.x + l.w * 0.5f;
        l.art = {l.x, kArtY, kArt, kArt};
        l.column_x = l.x + kArt + kColumnGap;
        l.column_w = l.w - kArt - kColumnGap;
        return l;
    }

    float queue_view_height() const
    {
        return kQueueH - kQueueListTop - 20.0f;
    }

    // Centre x of a control; "Use" sits at the right end of the row.
    float control_x(const Layout &l, int control) const
    {
        if (control == kUse)
            return l.x + l.w - kUseW * 0.5f;
        return l.cx + kControlOffset[control];
    }

    float thumb_x(const Layout &l) const
    {
        return l.x + tween::clamp01(shown_.value) * l.w;
    }

    // The ring: x relative to the row's centre (so it rides along when the
    // layout re-centres), y the centre, w / h its size.
    Rect ring_target() const
    {
        const Layout l = layout();
        if (zone_ == Zone::scrubber)
            return {thumb_x(l) - l.cx, kScrubY, 52.0f, 52.0f};
        if (button_ == kUse)
            return {control_x(l, kUse) - l.cx, kTransportY, kUseW + 18.0f, kUseH + 18.0f};
        const float d = ((button_ == kPlay ? kPlayRadius : kButtonRadius) + 9.0f) * 2.0f;
        return {kControlOffset[button_], kTransportY, d, d};
    }

    const Result *current() const
    {
        return results_.empty() ? nullptr : &results_[static_cast<std::size_t>(index_)];
    }

    void apply_palette(bool snap)
    {
        const Result fallback;
        const Result &r = current() != nullptr ? *current() : fallback;
        const Color targets[4] = {
            hui::gfx::mix(r.dark, Color::rgb(0x05060c), 0.45f),
            hui::gfx::mix(r.dark, r.mid, 0.3f),
            r.mid,
            hui::gfx::mix(r.mid, r.accent, 0.6f),
        };
        for (int i = 0; i < 4; ++i)
        {
            if (snap)
                palette_[i].snap(targets[i]);
            else
                palette_[i].target(targets[i]);
        }
        if (snap)
        {
            accent_.snap(r.accent);
            body_.snap(r.mid);
        }
        else
        {
            accent_.target(r.accent);
            body_.target(r.mid);
        }
    }

    // ---- input ------------------------------------------------------------------------
    void refuse(hui::ui::Feedback &feedback, float direction)
    {
        feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
        nudge_.trigger();
        nudge_direction_ = direction;
    }

    void set_zone(Zone zone, hui::ui::Feedback &feedback)
    {
        zone_ = zone;
        const Layout l = layout();
        if (zone == Zone::queue)
        {
            queue_row_index_ = index_;
            queue_row_.snap(static_cast<float>(index_));
            feedback.play(hui::audio::Cue::focus, 0.94f,
                          hui::ui::pan_for_x(kQueueX + kQueueW * 0.5f));
        }
        else
        {
            feedback.play(
                hui::audio::Cue::focus, zone == Zone::scrubber ? 1.1f : 1.0f,
                hui::ui::pan_for_x(zone == Zone::scrubber ? thumb_x(l) : control_x(l, button_)));
        }
    }

    void change_track(Context &context, int index, int travel)
    {
        if (results_.empty())
            return;
        previous_ = index_;
        index_ = index;
        travel_ = static_cast<float>(travel);
        shown_.snap(0.0f);
        swap_.start(context.settings.reduced_motion ? 0.14f : 0.6f);
        apply_palette(false);
        use_pending_ = false;
        message_.clear();
        start_stream(context);
    }

    void step_variant(Context &context, int delta, hui::ui::Feedback &feedback)
    {
        const int next = variant_ + delta;
        if (next < 0 || next >= kVariantCount || query_.empty() || searching_)
        {
            refuse(feedback, static_cast<float>(delta));
            return;
        }
        variant_ = next;
        feedback.play(hui::audio::Cue::tab, 1.0f, delta > 0 ? 0.4f : -0.4f);
        stop_playback(context);
        start_search();
    }

    void seek(Context &context, double delta, bool repeat, hui::ui::Feedback &feedback)
    {
        if (context.music == nullptr || !context.music->streaming())
        {
            if (!repeat)
                refuse(feedback, delta > 0.0 ? 1.0f : -1.0f);
            return;
        }
        // Only as far as the download has come.
        const double limit =
            std::max(0.0, std::min(context.music->buffered(), clip_seconds()) - 0.25);
        const double now = context.music->position();
        const double target = std::clamp(now + delta, 0.0, limit);
        if (std::fabs(target - now) < 0.5)
        {
            if (!repeat)
                refuse(feedback, delta > 0.0 ? 1.0f : -1.0f);
            return;
        }
        context.music->seek(target);
        const float fraction = static_cast<float>(target / clip_seconds());
        const Layout l = layout();
        feedback.play(hui::audio::Cue::slider, 0.85f + 0.4f * fraction,
                      hui::ui::pan_for_x(l.x + fraction * l.w));
        seek_.trigger();
        seek_hold_ = 1.1f;
    }

    void toggle_play(Context &context, hui::ui::Feedback &feedback)
    {
        if (results_.empty() || context.music == nullptr)
        {
            refuse(feedback, 0.0f);
            return;
        }
        const std::string &id = results_[static_cast<std::size_t>(index_)].video.id;
        const bool loaded = stream_ && stream_->id == id && context.music->streaming();
        if (!loaded || context.music->finished())
        {
            if (loaded && stream_->done.load() && stream_->ok)
                context.music->seek(0.0); // played to the end: from the top
            else
                start_stream(context);
            context.music->set_paused(false);
            playing_ = true;
        }
        else
        {
            playing_ = !playing_;
            context.music->set_paused(!playing_);
        }
        feedback.play(hui::audio::Cue::select, playing_ ? 1.0f : 0.9f,
                      hui::ui::pan_for_x(control_x(layout(), kPlay)));
    }

    void activate(Context &context, int control, hui::ui::Feedback &feedback)
    {
        press_[control].trigger();
        const float pan = hui::ui::pan_for_x(control_x(layout(), control));
        switch (control)
        {
        case kPlay:
            toggle_play(context, feedback);
            break;
        case kPrevious:
            if (position(context) > kRestartAfter && context.music != nullptr)
            {
                context.music->seek(0.0);
                feedback.play(hui::audio::Cue::tab, 0.94f, pan);
            }
            else if (index_ > 0)
            {
                change_track(context, index_ - 1, -1);
                feedback.play(hui::audio::Cue::tab, 0.94f, pan);
            }
            else
                refuse(feedback, -1.0f);
            break;
        case kNext:
            if (index_ + 1 < static_cast<int>(results_.size()))
            {
                change_track(context, index_ + 1, 1);
                feedback.play(hui::audio::Cue::tab, 1.0f, pan);
            }
            else
                refuse(feedback, 1.0f);
            break;
        case kUse:
            request_use(context, feedback);
            break;
        default:
            break;
        }
    }

    void update(Context &context, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback) override
    {
        if (finished_)
        {
            context.pop();
            return;
        }
        age_ += dt;
        clock_ += dt;
        if (!context.settings.reduced_motion)
            drift_ += dt;
        poll_search(context);
        pump_stream(context);
        poll_use(context);
        animate(context, dt);

        if (prompt_.is_open())
        {
            const hui::ui::Event event = prompt_.handle(input, feedback);
            if (event == hui::ui::Event::activated)
            {
                query_ = prompt_.text();
                if (!query_.empty())
                {
                    stop_playback(context);
                    start_search();
                }
            }
            else if (event == hui::ui::Event::cancelled && results_.empty() && !searching_)
            {
                leave(context);
                return;
            }
            prompt_.update(dt);
            return;
        }
        prompt_.update(dt);

        // Buttons that work from every zone.
        if (input.is_pressed(hui::Action::north)) // Triangle: search again
        {
            prompt_.open(feedback, query_);
            return;
        }
        if (input.is_pressed(hui::Action::west)) // Square: the queue
        {
            queue_open_ = !queue_open_;
            feedback.play(queue_open_ ? hui::audio::Cue::open : hui::audio::Cue::modal_close, 1.0f,
                          0.4f);
            if (!queue_open_ && zone_ == Zone::queue)
                zone_ = Zone::transport;
        }
        if (input.is_pressed(hui::Action::page_prev)) // L1 / R1: the next search
            step_variant(context, -1, feedback);
        if (input.is_pressed(hui::Action::page_next))
            step_variant(context, 1, feedback);
        if (input.is_pressed(hui::Action::jump_prev)) // L2 / R2: seek 15 s
            seek(context, -kSeekJump, false, feedback);
        if (input.is_pressed(hui::Action::jump_next))
            seek(context, kSeekJump, false, feedback);

        switch (zone_)
        {
        case Zone::scrubber:
            update_scrubber(context, input, feedback);
            break;
        case Zone::transport:
            if (input.is_pressed(hui::Action::back))
            {
                leave(context);
                return;
            }
            update_transport(context, input, feedback);
            break;
        case Zone::queue:
            update_queue(context, input, feedback);
            break;
        }
    }

    void leave(Context &context)
    {
        stop_playback(context);
        context.pop();
    }

    void update_transport(Context &context, const hui::InputFrame &input,
                          hui::ui::Feedback &feedback)
    {
        switch (input.nav)
        {
        case hui::Direction::left:
        case hui::Direction::right:
        {
            const int step = input.nav == hui::Direction::right ? 1 : -1;
            const int next = button_ + step;
            if (next >= 0 && next < kControls)
            {
                button_ = next;
                feedback.play(hui::audio::Cue::focus, 1.0f,
                              hui::ui::pan_for_x(control_x(layout(), next)));
            }
            else if (!input.nav_repeat)
            {
                if (step > 0 && queue_open_ && !results_.empty())
                    set_zone(Zone::queue, feedback);
                else
                    refuse(feedback, static_cast<float>(step));
            }
            break;
        }
        case hui::Direction::up:
            set_zone(Zone::scrubber, feedback);
            break;
        case hui::Direction::down:
            if (queue_open_ && !results_.empty())
                set_zone(Zone::queue, feedback);
            else if (!input.nav_repeat)
                refuse(feedback, 0.0f);
            break;
        case hui::Direction::none:
            break;
        }
        if (input.is_pressed(hui::Action::confirm))
            activate(context, button_, feedback);
    }

    void update_scrubber(Context &context, const hui::InputFrame &input,
                         hui::ui::Feedback &feedback)
    {
        switch (input.nav)
        {
        case hui::Direction::left:
            seek(context, -kSeekStep, input.nav_repeat, feedback);
            break;
        case hui::Direction::right:
            seek(context, kSeekStep, input.nav_repeat, feedback);
            break;
        case hui::Direction::down:
            set_zone(Zone::transport, feedback);
            break;
        case hui::Direction::up:
            if (!input.nav_repeat)
                refuse(feedback, 0.0f);
            break;
        case hui::Direction::none:
            break;
        }
        if (input.is_pressed(hui::Action::confirm))
            activate(context, kPlay, feedback);
        if (input.is_pressed(hui::Action::back))
        {
            zone_ = Zone::transport;
            feedback.play(hui::audio::Cue::back);
        }
    }

    void update_queue(Context &context, const hui::InputFrame &input, hui::ui::Feedback &feedback)
    {
        const int count = static_cast<int>(results_.size());
        switch (input.nav)
        {
        case hui::Direction::up:
        case hui::Direction::down:
        {
            const int next = queue_row_index_ + (input.nav == hui::Direction::down ? 1 : -1);
            if (next >= 0 && next < count)
            {
                queue_row_index_ = next;
                feedback.play(hui::audio::Cue::focus, 1.08f - 0.012f * static_cast<float>(next),
                              hui::ui::pan_for_x(kQueueX + kQueueW * 0.5f));
            }
            else if (!input.nav_repeat)
                refuse(feedback, 0.0f);
            break;
        }
        case hui::Direction::left:
            set_zone(Zone::transport, feedback);
            break;
        case hui::Direction::right:
            if (!input.nav_repeat)
                refuse(feedback, 1.0f);
            break;
        case hui::Direction::none:
            break;
        }
        if (input.is_pressed(hui::Action::confirm) && count > 0)
        {
            const int row = queue_row_index_;
            if (row == index_)
            {
                if (context.music != nullptr && context.music->streaming())
                    context.music->seek(0.0);
                else
                    start_stream(context);
            }
            else
                change_track(context, row, row > index_ ? 1 : -1);
            feedback.play(hui::audio::Cue::select, 1.0f,
                          hui::ui::pan_for_x(kQueueX + kQueueW * 0.5f));
        }
        if (input.is_pressed(hui::Action::back))
        {
            zone_ = Zone::transport;
            feedback.play(hui::audio::Cue::back);
        }
    }

    bool dev_inject_text(Context &context, const std::string &text) override
    {
        if (!prompt_.is_open())
            return false;
        query_ = text;
        prompt_.dismiss();
        if (!query_.empty())
        {
            stop_playback(context);
            start_search();
        }
        return true;
    }

    // ---- animation ---------------------------------------------------------------------
    void animate(Context &context, float dt)
    {
        const bool reduced = context.settings.reduced_motion;
        PreviewMusic *music = context.music;
        const bool streaming = music != nullptr && music->streaming();
        if (streaming && music->finished())
            playing_ = false;
        buffering_ = streaming && (music->starved() || (playing_ && music->buffered() < 0.05));
        const bool audible = streaming && playing_ && !buffering_;

        // The real spectrum of what is audible, metered like the concept's:
        // a fast attack, a slow fall and peak caps that hang for a moment.
        std::array<float, kBars> wanted{};
        if (audible)
            music->spectrum(wanted.data(), kBars);
        float bass = 0.0f;
        for (int i = 0; i < kBars; ++i)
        {
            float &level = levels_[static_cast<std::size_t>(i)];
            const float target = wanted[static_cast<std::size_t>(i)];
            const float rate = target > level ? 30.0f : 7.0f;
            level += (target - level) * (1.0f - std::exp(-rate * dt));
            float &peak = peaks_[static_cast<std::size_t>(i)];
            peak = std::max(level, peak - 0.7f * dt);
            if (i < 6)
                bass += level;
        }
        energy_ = bass / 6.0f;

        const float glide = reduced ? 40.0f : kSoft;
        const double clip = clip_seconds();
        shown_.target = streaming ? static_cast<float>(music->position() / clip) : 0.0f;
        shown_.update(dt, 14.0f);
        loaded_.target =
            streaming ? static_cast<float>(std::min(1.0, music->buffered() / clip)) : 0.0f;
        loaded_.update(dt, 8.0f);
        play_.target = audible ? 1.0f : 0.0f;
        play_.update(dt, glide);
        morph_.target = playing_ ? 1.0f : 0.0f;
        morph_.update(dt, reduced ? 40.0f : 16.0f);
        glow_.target = energy_;
        glow_.update(dt, 9.0f);
        queue_.target = queue_open_ ? 1.0f : 0.0f;
        queue_.update(dt, reduced ? 40.0f : 11.0f);
        swap_.update(dt);
        for (hui::ui::SpringColor &colour : palette_)
            colour.update(dt, 3.5f);
        accent_.update(dt, 4.0f);
        body_.update(dt, 4.0f);

        scrub_focus_.target = zone_ == Zone::scrubber ? 1.0f : 0.0f;
        scrub_focus_.update(dt, 16.0f);
        queue_focus_.target = zone_ == Zone::queue ? 1.0f : 0.0f;
        queue_focus_.update(dt, 16.0f);
        ring_.target(ring_target());
        ring_.update(dt, 18.0f);
        queue_row_.target = static_cast<float>(queue_row_index_);
        queue_row_.update(dt, 18.0f);
        const float row = static_cast<float>(zone_ == Zone::queue ? queue_row_index_ : index_);
        queue_scroll_.reveal(row * kQueueRow, (row + 1.0f) * kQueueRow, queue_view_height(),
                             kQueueRow, static_cast<float>(results_.size()) * kQueueRow);
        queue_scroll_.update(dt, 12.0f);

        nudge_.update(dt, 9.0f);
        seek_.update(dt, 6.0f);
        seek_hold_ = std::max(0.0f, seek_hold_ - dt);
        bubble_.target = zone_ == Zone::scrubber || seek_hold_ > 0.0f ? 1.0f : 0.0f;
        bubble_.update(dt, 16.0f);
        for (hui::ui::Pulse &press : press_)
            press.update(dt, 10.0f);
        spinner_.set_spinning(searching_ || buffering_ || converting_ || use_pending_, false);
        spinner_.update(dt);
        position_ = static_cast<float>(streaming ? music->position() : 0.0);
        buffered_ = static_cast<float>(streaming ? std::min(music->buffered(), clip) : 0.0);
        if (stream_)
        {
            const int total = stream_->total.load();
            download_ = total > 0 ? static_cast<float>(stream_->got.load()) / total : 0.0f;
        }
    }

    float appear(int order) const
    {
        return tween::stagger(age_, order, 0.07f, 0.55f);
    }

    float slide(Context &context, float in, float distance) const
    {
        return context.settings.reduced_motion ? 0.0f : distance * (1.0f - in);
    }

    // ---- drawing -----------------------------------------------------------------------
    bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const override
    {
        const Layout l = layout();
        hui::gfx::DrawList &list = scene.list;
        draw_header(context, list, l);
        draw_artwork(context, list, l);
        draw_info(context, list, l);
        draw_visualizer(list, l);
        draw_focus_ring(context, list, l, true);
        draw_scrubber(context, list, l);
        draw_transport(context, list, l);
        draw_focus_ring(context, list, l, false);
        draw_hints(context, list);
        bool glass = false;
        if (queue_.value > 0.01f)
        {
            glass = true;
            draw_queue(context, overlay.list, overlay.glass);
        }
        const bool modal = prompt_.visible();
        prompt_.draw(overlay);
        return glass || modal;
    }

    void draw_header(Context &context, hui::gfx::DrawList &list, const Layout &l) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const float in = appear(0);
        list.push_opacity(in);
        const float y = 116.0f - slide(context, in, 12.0f);
        const float w = hui::ui::text(list, fonts.display, "Theme Music", l.x, y, 32, kWhite);
        std::string line = query_.empty() ? std::string("from YouTube")
                                          : "\"" + printable(*fonts.regular.font, query_) + "\"";
        if (!results_.empty())
            line += "  \xC2\xB7  " + std::to_string(results_.size()) + " videos";
        hui::ui::text(list, fonts.regular, fonts.regular.font->fit(line, 22, l.w - w - 24 - 330),
                      l.x + w + 24, y, 22, kWhite.with_alpha(0.6f));
        // The search the results come from, between L1 and R1.
        if (!query_.empty())
        {
            const hui::ui::GlyphStyle style = hui::ui::GlyphStyle::dark();
            const float right = l.x + l.w;
            const float r1 = hui::ui::button_width(hui::ui::Button::r1, 30);
            hui::ui::draw_button(list, fonts, style, hui::ui::Button::r1, right - r1, y - 8, 30);
            const char *label = kVariants[variant_].label;
            const float lw = fonts.semibold.measure(label, 22);
            hui::ui::text(list, fonts.semibold, label, right - r1 - 14 - lw, y, 22,
                          gfx_accent().with_alpha(0.95f));
            const float l1 = hui::ui::button_width(hui::ui::Button::l1, 30);
            hui::ui::draw_button(list, fonts, style, hui::ui::Button::l1, right - r1 - 28 - lw - l1,
                                 y - 8, 30);
        }
        list.pop_opacity();
    }

    Color gfx_accent() const
    {
        return hui::gfx::mix(accent_.value(), kWhite, 0.3f);
    }

    void draw_cover(hui::gfx::DrawList &list, const Rect &art, int index, float alpha, float dx,
                    float live) const
    {
        if (alpha <= 0.01f)
            return;
        const Rect r{art.x + dx, art.y, art.w, art.h};
        list.push_opacity(alpha);
        list.shadow({r.x, r.y + 28, r.w, r.h}, kArtRadius, 50, Color::rgb(0x000000, 0.5f));
        const Color tint = hui::gfx::mix(Color::rgb(0xb9bcc8), kWhite, live);
        const Result *result = index >= 0 && index < static_cast<int>(results_.size())
                                   ? &results_[static_cast<std::size_t>(index)]
                                   : nullptr;
        if (result != nullptr && result->texture != 0)
        {
            list.image(result->texture, r, result->uv, tint, kArtRadius);
        }
        else
        {
            // No picture (none found yet, or the thumbnail host is out of
            // reach): the album's colours and a quaver.
            const Color body = result != nullptr ? result->mid : body_.value();
            list.gradient_rect(r, kArtRadius, hui::gfx::mix(body, kWhite, 0.12f),
                               hui::gfx::mix(body, Color::rgb(0x05060c), 0.45f));
            const float cx = r.cx();
            const float cy = r.cy() - 10.0f;
            const Color ink = kWhite.with_alpha(0.55f);
            list.circle(cx - 26.0f, cy + 52.0f, 34.0f, ink);
            list.rounded_rect({cx - 2.0f, cy - 78.0f, 16.0f, 132.0f}, 8.0f, ink);
            list.rotated_rect({cx + 4.0f, cy - 70.0f, 64.0f, 18.0f}, 9.0f, 0.6f, ink);
        }
        list.bordered_rect(r, kArtRadius, kClear, 2, kWhite.with_alpha(0.16f));
        list.pop_opacity();
    }

    void draw_artwork(Context &context, hui::gfx::DrawList &list, const Layout &l) const
    {
        const bool reduced = context.settings.reduced_motion;
        const float in = appear(1);
        const float live = play_.value;
        const float pulse = glow_.value;
        Rect art = l.art;
        art.x -= slide(context, in, 36.0f);
        const float scale =
            tween::lerp(kPausedScale, 1.0f, live) + (reduced ? 0.0f : 0.008f * pulse);
        list.push_opacity(in);
        list.push_transform(scale, art.cx(), art.cy(), 0, 0);
        const float lit = (0.14f + 0.36f * pulse) * (0.3f + 0.7f * live);
        list.glow(art.inset(-4), kArtRadius + 4, 90.0f + 60.0f * pulse,
                  hui::gfx::mix(accent_.value(), body_.value(), 0.25f).with_alpha(lit));
        if (swap_.running)
        {
            const float t = swap_.progress();
            const float leave = tween::clamp01(t * 2.3f);
            const float arrive = tween::clamp01((t - 0.2f) / 0.8f);
            const float distance = reduced ? 0.0f : 150.0f;
            draw_cover(list, art, previous_, 1.0f - tween::smoothstep(leave),
                       -travel_ * distance * tween::cubic_in(leave), live);
            draw_cover(list, art, index_, tween::smoothstep(arrive * 1.6f),
                       travel_ * distance * (1.0f - tween::quint_out(arrive)), live);
        }
        else
        {
            draw_cover(list, art, index_, 1.0f, 0.0f, live);
        }
        list.pop_transform();
        // Waiting on the network: the kit's spinner on the artwork.
        if (searching_ || buffering_)
        {
            hui::ui::Canvas canvas{list, context.fonts, 0, clock_};
            list.circle(art.cx(), art.cy(), 46.0f, Color::rgb(0x000000, 0.35f));
            spinner_.set_bounds({art.cx() - 26.0f, art.cy() - 26.0f, 52.0f, 52.0f});
            spinner_.draw(canvas);
        }
        list.pop_opacity();
    }

    void draw_track_text(Context &context, hui::gfx::DrawList &list, const Layout &l, int index,
                         float alpha, float dx) const
    {
        if (alpha <= 0.01f || index < 0 || index >= static_cast<int>(results_.size()))
            return;
        const hui::ui::Fonts &fonts = context.fonts;
        const Result &r = results_[static_cast<std::size_t>(index)];
        const float x = l.column_x + dx;
        list.push_opacity(alpha);
        // A long title wraps to two lines at a smaller size.
        const bool fits = fonts.display.measure(r.title, 64) <= l.column_w;
        if (fits)
            hui::ui::text(list, fonts.display, r.title, x - 3, 306, 64, kWhite);
        else
            hui::ui::paragraph(list, fonts.display, r.title, x - 2, 262, 46, l.column_w, 52, kWhite,
                               2);
        hui::ui::text(list, fonts.semibold, fonts.semibold.font->fit(r.author, 30, l.column_w), x,
                      356, 30, kWhite.with_alpha(0.92f));
        std::string line = "YouTube  \xC2\xB7  " + clock_text(static_cast<double>(r.video.seconds));
        line += "  \xC2\xB7  first " + clock_text(clip_seconds_for(r)) + " used";
        hui::ui::text(list, fonts.regular, fonts.regular.font->fit(line, 24, l.column_w), x, 396,
                      24, kWhite.with_alpha(0.62f));
        list.pop_opacity();
    }

    static double clip_seconds_for(const Result &r)
    {
        return r.video.seconds > 0 ? std::min<double>(r.video.seconds, at9::kMaxMusicSeconds)
                                   : at9::kMaxMusicSeconds;
    }

    void draw_info(Context &context, hui::gfx::DrawList &list, const Layout &l) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const float in = appear(2);
        const Color accent = gfx_accent();
        list.push_opacity(in);
        const float enter = slide(context, in, 30.0f);
        const float label_y = 206.0f;
        const float x = l.column_x + enter;

        // The state line.
        const char *state = searching_           ? "SEARCHING"
                            : results_.empty()   ? "NOTHING PLAYING"
                            : converting_        ? "CONVERTING"
                            : buffering_         ? "BUFFERING"
                            : play_.value > 0.5f ? "NOW PLAYING"
                            : playing_           ? "LOADING"
                                                 : "PAUSED";
        const bool lit = play_.value > 0.5f && !converting_;
        hui::ui::text(list, fonts.semibold, state, x, label_y, 18,
                      lit ? accent : kWhite.with_alpha(0.7f), hui::gfx::Align::left, 4.0f);
        if (!results_.empty())
        {
            char text[32];
            (void)std::snprintf(text, sizeof(text), "%02d / %02d", index_ + 1,
                                static_cast<int>(results_.size()));
            hui::ui::text(list, fonts.mono, text, l.column_x + l.column_w + enter, label_y, 20,
                          kWhite.with_alpha(0.6f), hui::gfx::Align::right);
        }

        if (results_.empty())
        {
            const char *title = searching_ ? "Listening around" : "Find a game's music";
            hui::ui::text(list, fonts.display, title, x - 3, 306, 64, kWhite);
            const std::string line =
                searching_
                    ? "Searching YouTube for " + std::string(kVariants[variant_].label) + "..."
                    : std::string("Press Triangle and type the game's name.");
            hui::ui::text(list, fonts.regular, fonts.regular.font->fit(line, 26, l.column_w), x,
                          356, 26, kWhite.with_alpha(0.7f));
        }
        else if (swap_.running)
        {
            const float t = swap_.progress();
            const float distance = context.settings.reduced_motion ? 0.0f : 1.0f;
            const float leave = tween::clamp01(t * 3.2f);
            const float arrive = tween::clamp01((t - 0.26f) / 0.74f);
            draw_track_text(context, list, l, previous_, 1.0f - tween::smoothstep(leave),
                            -travel_ * 40.0f * distance * tween::cubic_in(leave));
            draw_track_text(context, list, l, index_, tween::smoothstep(arrive),
                            travel_ * 56.0f * distance * (1.0f - tween::quint_out(arrive)));
        }
        else
        {
            draw_track_text(context, list, l, index_, 1.0f, enter);
        }
        // A problem, in the column under the track.
        if (!message_.empty())
            hui::ui::paragraph(list, fonts.regular, message_, x, 440, 22, l.column_w, 30, kWarn, 2);
        list.pop_opacity();
    }

    void draw_visualizer(hui::gfx::DrawList &list, const Layout &l) const
    {
        const float in = appear(3);
        const Color top = hui::gfx::mix(accent_.value(), kWhite, 0.4f);
        const Color bottom = hui::gfx::mix(body_.value(), accent_.value(), 0.45f);
        const float width = l.column_w / static_cast<float>(kBars) * 0.56f;
        const float pitch = (l.column_w - width) / static_cast<float>(kBars - 1);
        list.push_opacity(in);
        for (int i = 0; i < kBars; ++i)
        {
            const float grow = tween::stagger(age_, i, 0.012f, 0.5f);
            const float level = levels_[static_cast<std::size_t>(i)] * grow;
            const float h = width + level * (kBarsHeight - width);
            const float x = l.column_x + static_cast<float>(i) * pitch;
            const Color tip = hui::gfx::mix(bottom, top, 0.35f + 0.65f * level);
            list.gradient_rect({x, kBarsBase - h, width, h}, width * 0.5f, tip, bottom);
            const float mirror = width + (h - width) * 0.42f;
            list.gradient_rect({x, kBarsBase + 8.0f, width, mirror}, width * 0.5f,
                               bottom.with_alpha(0.3f), bottom.with_alpha(0.0f));
            const float peak = peaks_[static_cast<std::size_t>(i)] * grow;
            const float lifted = peak - level;
            if (lifted > 0.02f)
            {
                const float y = kBarsBase - width - peak * (kBarsHeight - width) - 8.0f;
                // Caps belong to the music: they go out with it.
                list.rounded_rect(
                    {x, y, width, 3.0f}, 1.5f,
                    top.with_alpha(tween::clamp01(lifted * 8.0f) * 0.55f * play_.value));
            }
        }
        list.pop_opacity();
    }

    void draw_scrubber(Context &context, hui::gfx::DrawList &list, const Layout &l) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const float in = appear(4);
        const float focus = scrub_focus_.value;
        const float thumb = thumb_x(l);
        const Color accent = hui::gfx::mix(accent_.value(), kWhite, 0.25f);
        list.push_opacity(in);
        const float y = kScrubY + slide(context, in, 24.0f);
        const float thick = 8.0f + 4.0f * focus;
        const Rect bar{l.x, y - thick * 0.5f, l.w, thick};
        list.rounded_rect(bar, thick * 0.5f, kWhite.with_alpha(0.16f + 0.06f * focus));
        // What has downloaded so far, like a video player's loaded bar.
        const float loaded = std::max(thick, tween::clamp01(loaded_.value) * l.w);
        list.rounded_rect({bar.x, bar.y, loaded, bar.h}, thick * 0.5f, kWhite.with_alpha(0.16f));
        const float filled = std::max(thick, thumb - l.x);
        list.glow({bar.x, bar.y, filled, bar.h}, thick * 0.5f, 10.0f + 8.0f * focus,
                  accent.with_alpha(0.22f + 0.2f * focus));
        list.gradient_rect_h({bar.x, bar.y, filled, bar.h}, thick * 0.5f,
                             hui::gfx::mix(body_.value(), accent, 0.5f), accent);
        const float radius = 9.0f + 6.0f * focus + 3.0f * seek_.value;
        list.shadow({thumb - radius, y - radius + 4, radius * 2, radius * 2}, radius, 10,
                    Color::rgb(0x000000, 0.45f));
        list.circle(thumb, y, radius, kWhite);

        hui::ui::text(list, fonts.mono, clock_text(position_), l.x, y + 46, 22,
                      kWhite.with_alpha(0.86f));
        hui::ui::text(list, fonts.mono, clock_text(clip_seconds()), l.x + l.w, y + 46, 22,
                      kWhite.with_alpha(0.6f), hui::gfx::Align::right);
        // In the middle: how much of the clip has arrived.
        if (!results_.empty() && buffered_ + 0.5f < clip_seconds())
        {
            char text[48];
            (void)std::snprintf(text, sizeof(text), "downloading %d%%",
                                static_cast<int>(download_ * 100.0f));
            hui::ui::text(list, fonts.regular, text, l.cx, y + 46, 20, kWhite.with_alpha(0.5f),
                          hui::gfx::Align::center);
        }

        const float bubble = tween::clamp01(bubble_.value);
        if (bubble > 0.01f)
        {
            const float cx = std::clamp(thumb, l.x + 44.0f, l.x + l.w - 44.0f);
            const Rect pill{cx - 44, y - 66 + 8.0f * (1.0f - bubble), 88, 36};
            list.push_opacity(bubble);
            list.shadow({pill.x, pill.y + 6, pill.w, pill.h}, 18, 14, Color::rgb(0x000000, 0.4f));
            list.rounded_rect(pill, 18, kWhite);
            hui::ui::text(list, fonts.mono, clock_text(shown_.value * clip_seconds() + 0.5),
                          pill.cx(), pill.cy() + 8, 22, kInk, hui::gfx::Align::center);
            list.pop_opacity();
        }
        list.pop_opacity();
    }

    void draw_skip_icon(hui::gfx::DrawList &list, float cx, float cy, float dir, Color c) const
    {
        side_triangle(list, cx - dir * 6.0f, cy, 24, dir, c);
        list.line(cx + dir * 12.0f, cy - 9, cx + dir * 12.0f, cy + 9, 4.5f, c);
    }

    void draw_transport(Context &context, hui::gfx::DrawList &list, const Layout &l) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const float in = appear(5);
        const Color accent = gfx_accent();
        list.push_opacity(in);
        const float y = kTransportY + slide(context, in, 28.0f);

        for (int i = kPrevious; i <= kNext; ++i)
        {
            const float cx = control_x(l, i);
            const bool focused = zone_ == Zone::transport && i == button_;
            list.push_transform(1.0f - 0.08f * press_[i].value, cx, y, 0, 0);
            if (i == kPlay)
            {
                const float live = play_.value;
                list.glow({cx - kPlayRadius, y - kPlayRadius, kPlayRadius * 2, kPlayRadius * 2},
                          kPlayRadius, 22.0f + 14.0f * glow_.value,
                          accent.with_alpha(0.2f + 0.3f * glow_.value * live));
                list.shadow(
                    {cx - kPlayRadius, y - kPlayRadius + 8, kPlayRadius * 2, kPlayRadius * 2},
                    kPlayRadius, 18, Color::rgb(0x000000, 0.4f));
                list.circle(cx, y, kPlayRadius, kWhite);
                const float m = morph_.value; // 1: playing, so the pause bars show
                if (m < 0.99f)
                {
                    const float s = 0.6f + 0.4f * (1.0f - m);
                    side_triangle(list, cx + 2.0f, y, 38.0f * s, 1.0f,
                                  hui::gfx::mix(kInk, kWhite, m));
                }
                if (m > 0.01f)
                {
                    const float s = 0.6f + 0.4f * m;
                    const Color c = hui::gfx::mix(kWhite, kInk, m);
                    list.rounded_rect({cx - 15 * s, y - 17 * s, 10 * s, 34 * s}, 4 * s, c);
                    list.rounded_rect({cx + 5 * s, y - 17 * s, 10 * s, 34 * s}, 4 * s, c);
                }
            }
            else
            {
                list.circle(cx, y, kButtonRadius, kWhite.with_alpha(focused ? 0.18f : 0.08f));
                list.ring(cx, y, kButtonRadius, 1.5f, kWhite.with_alpha(0.16f));
                const Color c = focused ? kWhite : Color::rgb(0xd4d7e2);
                draw_skip_icon(list, cx, y, i == kNext ? 1.0f : -1.0f, c);
            }
            list.pop_transform();
        }

        // Right end: the action this screen is for.
        {
            const float cx = control_x(l, kUse);
            const Rect pill{cx - kUseW * 0.5f, y - kUseH * 0.5f, kUseW, kUseH};
            const bool focused = zone_ == Zone::transport && button_ == kUse;
            list.push_transform(1.0f - 0.06f * press_[kUse].value, cx, y, 0, 0);
            list.shadow({pill.x, pill.y + 8, pill.w, pill.h}, kUseH * 0.5f, 18,
                        Color::rgb(0x000000, 0.35f));
            const bool busy = converting_ || use_pending_;
            list.rounded_rect(pill, kUseH * 0.5f,
                              focused ? kWhite
                                      : kWhite.with_alpha(results_.empty() ? 0.06f : 0.12f));
            list.bordered_rect(pill, kUseH * 0.5f, kClear, 1.5f, kWhite.with_alpha(0.2f));
            char label[48];
            if (converting_)
                (void)std::snprintf(label, sizeof(label), "Converting...");
            else if (use_pending_)
                (void)std::snprintf(label, sizeof(label), "Downloading %d%%",
                                    static_cast<int>(download_ * 100.0f));
            else
                (void)std::snprintf(label, sizeof(label), "Use this track");
            const Color ink = focused ? kInk : kWhite;
            float lx = pill.cx();
            if (busy)
            {
                hui::ui::Canvas canvas{list, fonts, 0, clock_};
                spinner_.set_bounds({pill.x + 22, pill.cy() - 13, 26, 26});
                spinner_.style.color = ink;
                spinner_.draw(canvas);
                spinner_.style.color = kWhite;
                lx += 14.0f;
            }
            hui::ui::text(list, fonts.semibold, label, lx, pill.cy() + 9, 24,
                          results_.empty() ? ink.with_alpha(0.4f) : ink, hui::gfx::Align::center);
            list.pop_transform();
        }

        // Left end: search again.
        const hui::ui::GlyphStyle style = hui::ui::GlyphStyle::dark();
        hui::ui::draw_button(list, fonts, style, hui::ui::Button::triangle, l.x, y, 34);
        hui::ui::text(list, fonts.semibold, "Search", l.x + 48, y + 9, 24, kWhite.with_alpha(0.8f));
        list.pop_opacity();
    }

    void draw_focus_ring(Context &context, hui::gfx::DrawList &list, const Layout &l,
                         bool light) const
    {
        const float alpha = appear(5) * (1.0f - queue_focus_.value);
        if (alpha <= 0.01f)
            return;
        const Rect ring = ring_.value();
        const float lock = scrub_focus_.value;
        const bool sideways = nudge_direction_ != 0.0f;
        const float cx =
            tween::lerp(l.cx + ring.x, thumb_x(l), lock) +
            (sideways ? hui::ui::shake(nudge_.value, clock_, 12.0f) * nudge_direction_ : 0.0f);
        const float cy = ring.y + slide(context, appear(5), 28.0f) +
                         (sideways ? 0.0f : hui::ui::shake(nudge_.value, clock_, 8.0f));
        // A circle on the round buttons, a capsule on the pill.
        const Rect r{cx - ring.w * 0.5f, cy - ring.h * 0.5f, ring.w, ring.h};
        const float radius = std::min(ring.w, ring.h) * 0.5f;
        list.push_opacity(alpha);
        if (light)
            list.glow(r, radius, 18,
                      gfx_accent().with_alpha(0.34f + 0.2f * hui::ui::breathe(clock_)));
        else
            list.bordered_rect(r, radius, kClear, 3.5f, kWhite);
        list.pop_opacity();
    }

    // The three-bar equalizer on the playing row, reading the real spectrum.
    void draw_equalizer(hui::gfx::DrawList &list, float cx, float cy, Color color) const
    {
        constexpr int kSource[3] = {2, 9, 17};
        for (int i = 0; i < 3; ++i)
        {
            const float h = 5.0f + 17.0f * levels_[static_cast<std::size_t>(kSource[i])];
            list.rounded_rect({cx - 10.0f + static_cast<float>(i) * 8.0f, cy + 10.0f - h, 5, h},
                              2.5f, color);
        }
    }

    void draw_queue(Context &context, hui::gfx::DrawList &list, std::uint32_t glass) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const float in = appear(6);
        const float open = queue_.value * (context.settings.reduced_motion ? 1.0f : in);
        const Color accent = gfx_accent();
        const Rect panel{kQueueX + (1.0f - open) * (kQueueW + kMargin + 60.0f), kQueueY, kQueueW,
                         kQueueH};
        list.push_opacity(tween::clamp01(queue_.value * 1.5f) * in);
        list.shadow({panel.x, panel.y + 20, panel.w, panel.h}, kQueueRadius, 56,
                    Color::rgb(0x000000, 0.42f));
        if (glass != 0)
            list.glass(glass, panel, kQueueRadius, kWhite);
        list.rounded_rect(
            panel, kQueueRadius,
            hui::gfx::mix(palette_[0].value(), Color::rgb(0x0b0d16), 0.4f).with_alpha(0.5f));
        list.bordered_rect(panel, kQueueRadius, kClear, 1.5f, kWhite.with_alpha(0.2f));

        hui::ui::text(list, fonts.semibold, "RESULTS", panel.x + 32, panel.y + 56, 18, accent,
                      hui::gfx::Align::left, 4.0f);
        hui::ui::text(list, fonts.regular, kVariants[variant_].label, panel.x + panel.w - 32,
                      panel.y + 56, 20, kWhite.with_alpha(0.6f), hui::gfx::Align::right);
        list.rounded_rect({panel.x + 32, panel.y + 84, panel.w - 64, 1.5f}, 0.75f,
                          kWhite.with_alpha(0.12f));

        const float top = panel.y + kQueueListTop - 20.0f;
        const float view = queue_view_height() + 20.0f;
        const float origin = panel.y + kQueueListTop - queue_scroll_.offset();
        list.push_clip({panel.x, top, panel.w, view});
        const int count = static_cast<int>(results_.size());
        if (count == 0)
        {
            hui::ui::paragraph(list, fonts.regular,
                               searching_ ? "Looking it up..." : "Search results appear here.",
                               panel.x + 32, origin + 36, 22, panel.w - 64, 30,
                               kWhite.with_alpha(0.6f), 2);
        }
        const float inset = 14.0f;
        const float focus = queue_focus_.value;
        if (count > 0)
        {
            const Rect playing{panel.x + inset, origin + static_cast<float>(index_) * kQueueRow + 3,
                               panel.w - 2 * inset, kQueueRow - 6};
            list.rounded_rect(playing, 18, kWhite.with_alpha(0.07f));
        }
        if (focus > 0.01f && count > 0)
        {
            Rect row{panel.x + inset, origin + queue_row_.value * kQueueRow + 3,
                     panel.w - 2 * inset, kQueueRow - 6};
            row.x += hui::ui::shake(nudge_.value, clock_, 10.0f, 9.0f) * nudge_direction_;
            row.y +=
                nudge_direction_ == 0.0f ? hui::ui::shake(nudge_.value, clock_, 7.0f, 9.0f) : 0.0f;
            list.glow(row, 18, 14, accent.with_alpha(0.22f * focus));
            list.rounded_rect(row, 18, kWhite.with_alpha(0.16f * focus));
            list.bordered_rect(row, 18, kClear, 2.5f, kWhite.with_alpha(focus));
        }
        const auto row_alpha = [&](int i)
        {
            const float y = origin + static_cast<float>(i) * kQueueRow;
            return tween::clamp01((y - top + 24.0f) / 36.0f) *
                   tween::clamp01((top + view - (y + kQueueRow) + 24.0f) / 36.0f) *
                   tween::stagger(age_, i, 0.03f, 0.4f);
        };
        char text[32];
        for (int i = 0; i < count; ++i)
        {
            const float alpha = row_alpha(i);
            if (alpha <= 0.01f)
                continue;
            const Result &r = results_[static_cast<std::size_t>(i)];
            const bool current_row = i == index_;
            const bool focused = zone_ == Zone::queue && i == queue_row_index_;
            const float cy = origin + (static_cast<float>(i) + 0.5f) * kQueueRow;
            list.push_opacity(alpha);
            if (current_row && play_.value > 0.5f)
                draw_equalizer(list, panel.x + 50, cy, accent);
            else
            {
                (void)std::snprintf(text, sizeof(text), "%02d", i + 1);
                hui::ui::text(list, fonts.mono, text, panel.x + 50, cy + 7, 20,
                              kWhite.with_alpha(current_row ? 0.9f : 0.5f),
                              hui::gfx::Align::center);
            }
            const std::string time = clock_text(static_cast<double>(r.video.seconds));
            hui::ui::text(list, fonts.mono, time, panel.x + panel.w - 34, cy + 7, 20,
                          kWhite.with_alpha(current_row ? 0.9f : 0.6f), hui::gfx::Align::right);
            const hui::ui::FontRef &face = current_row ? fonts.semibold : fonts.regular;
            hui::ui::text(list, face, face.font->fit(r.title, 24, panel.w - 84 - 108), panel.x + 84,
                          cy + 8, 24, kWhite.with_alpha(current_row || focused ? 1.0f : 0.8f));
            list.pop_opacity();
        }
        list.pop_clip();
        list.pop_opacity();
    }

    void draw_hints(Context &context, hui::gfx::DrawList &list) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const hui::ui::GlyphStyle style = hui::ui::GlyphStyle::dark();
        constexpr const char *kLabels[kControls] = {"Previous", "Play", "Next", "Use this track"};
        const char *play = playing_ ? "Pause" : "Play";
        const char *queue = queue_open_ ? "Hide results" : "Show results";
        list.push_opacity(appear(8));
        switch (zone_)
        {
        case Zone::scrubber:
        {
            const hui::ui::Hint hints[] = {{hui::ui::Button::dpad, "Seek 5 s"},
                                           {hui::ui::Button::cross, play},
                                           {hui::ui::Button::square, queue},
                                           {hui::ui::Button::l2, "Seek 15 s", hui::ui::Button::r2}};
            hui::ui::draw_hints(list, fonts, style, hints, 4, 1824, true);
            break;
        }
        case Zone::transport:
        {
            const hui::ui::Hint hints[] = {
                {hui::ui::Button::cross, button_ == kPlay ? play : kLabels[button_]},
                {hui::ui::Button::square, queue},
                {hui::ui::Button::l1, "Other search", hui::ui::Button::r1},
                {hui::ui::Button::circle, "Back"}};
            hui::ui::draw_hints(list, fonts, style, hints, 4, 1824, true);
            break;
        }
        case Zone::queue:
        {
            const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "Play track"},
                                           {hui::ui::Button::circle, "Back"},
                                           {hui::ui::Button::square, queue}};
            hui::ui::draw_hints(list, fonts, style, hints, 3, 1824, true);
            break;
        }
        }
        list.pop_opacity();
    }

    std::span<const hui::ui::Hint> hints() const override
    {
        return {}; // drawn here, in the concept's own manner
    }

  private:
    std::string query_; // the game's name
    std::string search_term_;
    std::function<void(std::vector<unsigned char>)> on_at9_;
    bool started_ = false;
    bool finished_ = false;
    int variant_ = 0;
    std::string message_;

    // ---- work ----
    std::shared_ptr<SearchJob> search_;
    std::shared_ptr<StreamJob> stream_;
    std::shared_ptr<ConvertJob> convert_;
    bool searching_ = false;
    bool converting_ = false;
    bool use_pending_ = false;   // "Use" pressed; waiting for the download
    std::size_t fed_frames_ = 0; // source frames handed to the mixer
    bool stream_ended_ = false;

    // ---- the results and what plays ----
    std::vector<Result> results_;
    int index_ = 0;
    int previous_ = 0;
    float travel_ = 1.0f;
    bool playing_ = false;   // wanted: the user has not paused
    bool buffering_ = false; // playing, but waiting on the download
    float position_ = 0.0f;  // seconds audible
    float buffered_ = 0.0f;  // seconds decoded (within the clip)
    float download_ = 0.0f;  // 0..1 of the download

    // ---- focus ----
    Zone zone_ = Zone::transport;
    int button_ = kPlay;
    bool queue_open_ = true;
    int queue_row_index_ = 0;

    // ---- animation ----
    float age_ = 0.0f;
    float clock_ = 0.0f;
    float drift_ = 0.0f;
    std::array<float, kBars> levels_{};
    std::array<float, kBars> peaks_{};
    float energy_ = 0.0f;
    tween::Spring glow_;
    tween::Spring shown_;
    tween::Spring loaded_;
    tween::Spring play_;
    tween::Spring morph_;
    tween::Spring queue_;
    tween::Timer swap_;
    hui::ui::SpringColor palette_[4];
    hui::ui::SpringColor accent_;
    hui::ui::SpringColor body_;
    tween::Spring scrub_focus_;
    tween::Spring queue_focus_;
    hui::ui::SpringRect ring_;
    tween::Spring queue_row_;
    hui::ui::Scroller queue_scroll_;
    hui::ui::Pulse nudge_;
    float nudge_direction_ = 0.0f;
    hui::ui::Pulse seek_;
    float seek_hold_ = 0.0f;
    tween::Spring bubble_;
    hui::ui::Pulse press_[kControls];

    mutable hui::ui::InputPrompt prompt_;
    mutable hui::ui::Spinner spinner_;
};

} // namespace

std::unique_ptr<Screen>
make_music_search_screen(Context &context, std::string query,
                         std::function<void(std::vector<unsigned char>)> on_at9)
{
    return std::make_unique<MusicSearchScreen>(context, std::move(query), std::move(on_at9));
}

} // namespace fwd
