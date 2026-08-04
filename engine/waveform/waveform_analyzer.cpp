#include "waveform_analyzer.hpp"

namespace aud::waveform {

Result<void> WaveformAnalyzer::begin(const AudioSpec& spec) {
    m_store->reset(spec.channels);
    return {};
}

Result<void> WaveformAnalyzer::process(const ChunkView& chunk) {
    for (std::size_t ch = 0; ch < chunk.channels.size(); ++ch) {
        m_store->appendChunk(static_cast<ChannelIndex>(ch), chunk.channels[ch]);
    }
    return {};
}

Result<AnalysisResult> WaveformAnalyzer::finish() {
    m_store->markComplete();
    return AnalysisResult{};
}

std::unique_ptr<Analyzer> makeWaveformAnalyzer(WaveformStore& store) {
    return std::make_unique<WaveformAnalyzer>(store);
}

}  // namespace aud::waveform
