#pragma once

// The streaming Analyzer shape, fixed once here so M08-M14's analysers and M04's waveform
// generator all conform to the same interface — M20 formalises registration/dispatch around it,
// not the shape itself. See M00 §6 "Analyzer shape".

#include <cstdint>
#include <span>
#include <string_view>

#include "../util/audio_types.hpp"
#include "../util/result.hpp"

namespace aud {

// What begin() is told about the source before any chunk arrives. frameCount may be kNoFrame if
// the container didn't report a total (M02 progressive decode) — analysers that need a firm total
// up front must handle that by growing geometrically rather than failing.
struct AudioSpec {
    SampleRate   sampleRate = 0;
    ChannelIndex channels   = 0;
    FrameIndex   frameCount = kNoFrame;
};

// One chunk of planar PCM, handed to process() in order. `channels[c]` are all the same length
// (chunk.frameCount()); the view is non-owning and only valid for the duration of the process()
// call it's passed to.
struct ChunkView {
    std::span<const std::span<const Sample>> channels;
    FrameIndex                               startFrame = 0;

    [[nodiscard]] std::size_t frameCount() const noexcept {
        return channels.empty() ? 0 : channels[0].size();
    }
};

// Placeholder result envelope. One-shot analysers return whatever payload their finish() produces
// through their own accessor (e.g. WaveformAnalyzer's WaveformStore) rather than through this type
// today; M20's registry is what will need a real closed variant here to dispatch generically.
struct AnalysisResult {
    bool ok = true;
};

class Analyzer {
public:
    virtual ~Analyzer() = default;

    virtual std::string_view id()      const noexcept = 0;  // e.g. "waveform.peakrms"
    virtual std::uint32_t    version() const noexcept = 0;  // bump invalidates cache (M16)

    virtual Result<void>          begin(const AudioSpec& spec)    = 0;  // once, before any chunk
    virtual Result<void>          process(const ChunkView& chunk) = 0;  // called in order, once per chunk
    virtual Result<AnalysisResult> finish()                       = 0;  // once, after the last chunk

    virtual bool        isParallelisable() const noexcept { return false; }
    virtual std::size_t requiredLookbackFrames() const noexcept { return 0; }
};

}  // namespace aud
