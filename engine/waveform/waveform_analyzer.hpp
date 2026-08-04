#pragma once

// The streaming Analyzer (M00 §6 shape) that drives WaveformStore. Deliberately thin: all the
// storage/derived-variant logic lives in WaveformStore so it can also be populated for random
// re-derivation of mono-sum/mid-side bins outside the begin/process/finish sequence (M04 "Stereo
// presentation modes" — those are computed lazily from the AudioBuffer, not from process()).

#include <cstdint>
#include <memory>
#include <string_view>

#include "../analysis/analyzer.hpp"
#include "waveform_store.hpp"

namespace aud::waveform {

class WaveformAnalyzer final : public Analyzer {
public:
    // Non-owning: `store` must outlive this analyser and every chunk passed to process().
    explicit WaveformAnalyzer(WaveformStore& store) noexcept : m_store(&store) {}

    [[nodiscard]] std::string_view id() const noexcept override { return "waveform.peakrms"; }
    [[nodiscard]] std::uint32_t    version() const noexcept override { return 1; }

    Result<void>           begin(const AudioSpec& spec) override;
    Result<void>           process(const ChunkView& chunk) override;
    Result<AnalysisResult> finish() override;

    // Chunks are independent once bin-aligned (see waveform_bin.hpp's static_assert), so each
    // channel of each chunk could in principle be reduced on a different worker (M20).
    [[nodiscard]] bool isParallelisable() const noexcept override { return true; }

private:
    WaveformStore* m_store;
};

// Factory: builds a WaveformAnalyzer behind the Analyzer interface, with the `new` entirely
// inside aud_core's own -fno-rtti-flagged compilation. aud_core is built PRIVATE -fno-rtti (M00
// §2), but consumers outside it (tests, the Embind bindings target) are not — directly naming and
// constructing a concrete aud_core polymorphic type from one of those RTTI-enabled TUs asks the
// linker for "typeinfo for WaveformAnalyzer", which a -fno-rtti compile of its key function
// (begin(), in waveform_analyzer.cpp) never emits, and the link fails. Going through this factory
// and interacting only through the returned Analyzer* sidesteps that entirely; deletion through a
// virtual destructor is always safe across this boundary regardless of RTTI settings.
[[nodiscard]] std::unique_ptr<Analyzer> makeWaveformAnalyzer(WaveformStore& store);

}  // namespace aud::waveform
