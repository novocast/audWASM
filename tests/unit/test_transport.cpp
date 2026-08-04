#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "../../engine/playback/transport.hpp"
#include "../../engine/util/audio_buffer.hpp"

using aud::AudioBuffer;
using aud::ChannelIndex;
using aud::FrameRange;
using aud::Sample;
using aud::playback::RingBuffer;
using aud::playback::Resampler;
using aud::playback::Transport;
using aud::playback::TransportEvent;
using aud::playback::TransportStatus;

namespace {

AudioBuffer makeMonoBuffer(std::size_t frames, aud::SampleRate rate = 44100) {
    auto result = AudioBuffer::create(rate, 1);
    REQUIRE(result.has_value());
    auto                 buffer = std::move(result).value();
    std::vector<Sample>  data(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        data[i] = static_cast<Sample>(i);  // ramp, so we can identify exact source positions
    }
    std::vector<std::span<const Sample>> planar{data};
    REQUIRE(buffer.append(planar, frames).has_value());
    return buffer;
}

}  // namespace

TEST_CASE("Transport exhaustive-ish transition table", "[transport]") {
    using Status = TransportStatus;

    struct Case {
        Status status;
        bool   loadOk, readyOk, playOk, pauseOk, seekOk, endReachedOk;
    };

    // clang-format off
    const std::vector<Case> cases = {
        {Status::Idle,    true,  false, false, false, false, false},
        {Status::Loading, false, true,  false, false, false, false},
        {Status::Ready,   false, false, true,  false, true,  false},
        {Status::Playing, false, false, true,  true,  true,  true },
        {Status::Paused,  false, false, true,  true,  true,  false},
        {Status::Ended,   true,  false, false, false, true,  false},
    };
    // clang-format on

    for (const auto& c : cases) {
        auto reachStatus = [&](Transport& t) {
            switch (c.status) {
                case Status::Idle: return;
                case Status::Loading: REQUIRE(t.dispatch(TransportEvent::load()).has_value()); return;
                case Status::Ready:
                    REQUIRE(t.dispatch(TransportEvent::load()).has_value());
                    REQUIRE(t.dispatch(TransportEvent::readySignal(1000)).has_value());
                    return;
                case Status::Playing:
                    REQUIRE(t.dispatch(TransportEvent::load()).has_value());
                    REQUIRE(t.dispatch(TransportEvent::readySignal(1000)).has_value());
                    REQUIRE(t.dispatch(TransportEvent::play()).has_value());
                    return;
                case Status::Paused:
                    REQUIRE(t.dispatch(TransportEvent::load()).has_value());
                    REQUIRE(t.dispatch(TransportEvent::readySignal(1000)).has_value());
                    REQUIRE(t.dispatch(TransportEvent::play()).has_value());
                    REQUIRE(t.dispatch(TransportEvent::pause()).has_value());
                    return;
                case Status::Ended:
                    REQUIRE(t.dispatch(TransportEvent::load()).has_value());
                    REQUIRE(t.dispatch(TransportEvent::readySignal(1000)).has_value());
                    REQUIRE(t.dispatch(TransportEvent::play()).has_value());
                    REQUIRE(t.dispatch(TransportEvent::endReached()).has_value());
                    return;
                default: return;
            }
        };

        {
            Transport t(1, 44100, 44100);
            reachStatus(t);
            REQUIRE(t.dispatch(TransportEvent::load()).has_value() == c.loadOk);
        }
        {
            Transport t(1, 44100, 44100);
            reachStatus(t);
            REQUIRE(t.dispatch(TransportEvent::readySignal(500)).has_value() == c.readyOk);
        }
        {
            Transport t(1, 44100, 44100);
            reachStatus(t);
            REQUIRE(t.dispatch(TransportEvent::play()).has_value() == c.playOk);
        }
        {
            Transport t(1, 44100, 44100);
            reachStatus(t);
            REQUIRE(t.dispatch(TransportEvent::pause()).has_value() == c.pauseOk);
        }
        {
            Transport t(1, 44100, 44100);
            reachStatus(t);
            REQUIRE(t.dispatch(TransportEvent::seekTo(10)).has_value() == c.seekOk);
        }
        {
            Transport t(1, 44100, 44100);
            reachStatus(t);
            REQUIRE(t.dispatch(TransportEvent::endReached()).has_value() == c.endReachedOk);
        }
    }
}

TEST_CASE("Transport config setters (loop/gain) always succeed regardless of status", "[transport]") {
    Transport t(1, 44100, 44100);
    REQUIRE(t.dispatch(TransportEvent::setLoopRange(FrameRange{10, 20})).has_value());
    REQUIRE(t.dispatch(TransportEvent::setLoopEnabled(true)).has_value());
    REQUIRE(t.dispatch(TransportEvent::setLoopCrossfadeFrames(64)).has_value());
    REQUIRE(t.dispatch(TransportEvent::setGain(0.5f)).has_value());
    REQUIRE(t.state().loop.enabled);
    REQUIRE(t.state().loop.range.begin == 10);
    REQUIRE(t.state().gain == 0.5f);
}

TEST_CASE("Transport rejects non-1.0 playback rate as not implemented", "[transport]") {
    Transport t(1, 44100, 44100);
    auto      result = t.dispatch(TransportEvent::setRate(1.5));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == aud::ErrorCode::NotImplemented);
    REQUIRE(t.dispatch(TransportEvent::setRate(1.0)).has_value());
}

TEST_CASE("Transport produceInto pulls sequential frames into the ring at matching rates", "[transport]") {
    auto buffer = makeMonoBuffer(1000);

    Transport t(1, 44100, 44100);
    t.setSource(&buffer);
    t.setSourceComplete(true);
    REQUIRE(t.dispatch(TransportEvent::load()).has_value());
    REQUIRE(t.dispatch(TransportEvent::readySignal(1000)).has_value());
    REQUIRE(t.dispatch(TransportEvent::play()).has_value());

    RingBuffer  ring(1, 2048);
    // Ask for more than the buffer holds so produceInto actually runs off the end within this one
    // call (and so we can observe the Ended transition it triggers).
    std::size_t produced = t.produceInto(ring, 1200);
    REQUIRE(produced == 1000);

    std::vector<Sample>            out(1000);
    std::vector<std::span<Sample>> outSpans{std::span<Sample>(out)};
    REQUIRE(ring.read(std::span<std::span<Sample>>(outSpans), 1000) == 1000);
    for (std::size_t i = 0; i < 1000; ++i) {
        REQUIRE(out[i] == static_cast<Sample>(i));
    }
    REQUIRE(t.state().status == TransportStatus::Ended);
}

TEST_CASE("Transport seek jumps the read cursor and resets the resampler", "[transport]") {
    auto buffer = makeMonoBuffer(1000);

    Transport t(1, 44100, 44100);
    t.setSource(&buffer);
    t.setSourceComplete(true);
    REQUIRE(t.dispatch(TransportEvent::load()).has_value());
    REQUIRE(t.dispatch(TransportEvent::readySignal(1000)).has_value());
    REQUIRE(t.dispatch(TransportEvent::play()).has_value());
    REQUIRE(t.dispatch(TransportEvent::seekTo(500)).has_value());
    // Seeking is a transient overlay (M03): dispatch() itself only records the target and flips to
    // Seeking. It resolves back to the pre-seek status (Playing here) inside the next
    // produceInto() call, once frames have actually been produced from the new position.
    REQUIRE(t.state().status == TransportStatus::Seeking);

    RingBuffer  ring(1, 1024);
    std::size_t produced = t.produceInto(ring, 200);
    REQUIRE(produced == 200);
    REQUIRE(t.state().status == TransportStatus::Playing);

    std::vector<Sample>            out(200);
    std::vector<std::span<Sample>> outSpans{std::span<Sample>(out)};
    REQUIRE(ring.read(std::span<std::span<Sample>>(outSpans), 200) == 200);

    // The first render quantum after a seek is cosine-ramped in (M03 "Seeking" step 4), so sample 0
    // is muted; well past the ramp window the raw ramp values (500 + i) come through unscaled.
    REQUIRE(out[0] == 0.0f);
    REQUIRE(out[150] == 650.0f);
}

TEST_CASE("Transport ramps in the first render quantum after a seek (click-free)", "[transport]") {
    auto buffer = makeMonoBuffer(1000);

    Transport t(1, 44100, 44100);
    t.setSource(&buffer);
    t.setSourceComplete(true);
    REQUIRE(t.dispatch(TransportEvent::load()).has_value());
    REQUIRE(t.dispatch(TransportEvent::readySignal(1000)).has_value());
    REQUIRE(t.dispatch(TransportEvent::play()).has_value());
    REQUIRE(t.dispatch(TransportEvent::seekTo(500)).has_value());

    RingBuffer  ring(1, 1024);
    std::size_t produced = t.produceInto(ring, 130);
    REQUIRE(produced == 130);

    std::vector<Sample>            out(130);
    std::vector<std::span<Sample>> outSpans{std::span<Sample>(out)};
    REQUIRE(ring.read(std::span<std::span<Sample>>(outSpans), 130) == 130);

    REQUIRE(out[0] == 0.0f);              // fully muted at the discontinuity
    REQUIRE(out[1] > 0.0f);               // ramping up
    REQUIRE(out[1] < out[64]);            // monotonically increasing through the ramp
    REQUIRE(out[127] > out[64]);
    REQUIRE(out[129] == 629.0f);          // past the 128-frame ramp window: unscaled
}

TEST_CASE("Transport loops within a range without advancing past loop end", "[transport]") {
    auto buffer = makeMonoBuffer(1000);

    Transport t(1, 44100, 44100);
    t.setSource(&buffer);
    t.setSourceComplete(true);
    // Loop range starts at frame 0, matching the read cursor Load() already sets, so no seek (and
    // therefore no fade-in ramp) is needed to reach it — keeps this test focused on wrap behaviour.
    REQUIRE(t.dispatch(TransportEvent::load()).has_value());
    REQUIRE(t.dispatch(TransportEvent::readySignal(1000)).has_value());
    REQUIRE(t.dispatch(TransportEvent::setLoopRange(FrameRange{0, 50})).has_value());
    REQUIRE(t.dispatch(TransportEvent::setLoopEnabled(true)).has_value());
    REQUIRE(t.dispatch(TransportEvent::play()).has_value());

    RingBuffer  ring(1, 4096);
    std::size_t produced = t.produceInto(ring, 260);  // > 5 loop iterations of 50 frames each
    REQUIRE(produced == 260);

    std::vector<Sample>            out(260);
    std::vector<std::span<Sample>> outSpans{std::span<Sample>(out)};
    REQUIRE(ring.read(std::span<std::span<Sample>>(outSpans), 260) == 260);

    for (std::size_t i = 0; i < 260; ++i) {
        const auto expected = static_cast<Sample>(i % 50);
        REQUIRE(out[i] == expected);
    }
    // Looping forever: status must still be Playing, never Ended.
    REQUIRE(t.state().status == TransportStatus::Playing);
}

TEST_CASE("Transport enters Loading when source is still decoding past the current frame count", "[transport]") {
    auto buffer = makeMonoBuffer(100);  // only 100 frames decoded so far

    Transport t(1, 44100, 44100);
    t.setSource(&buffer);
    t.setSourceComplete(false);  // decode still in progress
    REQUIRE(t.dispatch(TransportEvent::load()).has_value());
    REQUIRE(t.dispatch(TransportEvent::readySignal(aud::kNoFrame)).has_value());
    REQUIRE(t.dispatch(TransportEvent::play()).has_value());

    RingBuffer  ring(1, 1024);
    std::size_t produced = t.produceInto(ring, 500);
    REQUIRE(produced == 100);
    REQUIRE(t.state().status == TransportStatus::Loading);
}
