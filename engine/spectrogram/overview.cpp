#include "overview.hpp"

#include <vector>

#include "frame_computer.hpp"
#include "freq_mapping.hpp"

namespace aud::spectrogram {

Result<OverviewStrip> computeOverviewStrip(const AudioBuffer& buffer, ChannelIndex channel,
                                            const TileConfig& config, std::uint32_t width, std::uint32_t height) {
    if (channel >= buffer.channelCount()) {
        return Error{ErrorCode::InvalidArgument, "spectrogram.overview", "channel out of range"};
    }
    if (width == 0 || height == 0) {
        return Error{ErrorCode::InvalidArgument, "spectrogram.overview", "width/height must be > 0"};
    }

    AUD_TRY_ASSIGN(frameComputer,
                   FrameComputer::create(config.fftSize, static_cast<aud::fft::WindowType>(config.window),
                                          static_cast<aud::fft::SpectrumScaling>(config.scaling), buffer.sampleRate()));
    AUD_TRY_ASSIGN(mapping, FreqMapping::create(static_cast<FreqAxis>(config.freqAxis), config.minHz,
                                                 static_cast<double>(buffer.sampleRate()), frameComputer.binCount(),
                                                 height));

    OverviewStrip strip;
    strip.width   = width;
    strip.height  = height;
    strip.floorDb = config.floorDb;
    strip.ceilDb  = config.ceilDb;
    strip.pixels.assign(static_cast<std::size_t>(width) * height, 0);

    const auto          total = buffer.frameCount();
    std::vector<double> rowScratch(height);

    for (std::uint32_t col = 0; col < width; ++col) {
        const double     frac   = (static_cast<double>(col) + 0.5) / static_cast<double>(width);
        const FrameIndex center = static_cast<FrameIndex>(frac * static_cast<double>(total));

        const auto magnitudes = frameComputer.computeFrame(buffer, channel, center);
        mapping.mapToDb(magnitudes, rowScratch);

        for (std::uint32_t row = 0; row < height; ++row) {
            strip.pixels[static_cast<std::size_t>(row) * width + col] =
                quantiseDb(rowScratch[row], strip.floorDb, strip.ceilDb);
        }
    }

    return strip;
}

}  // namespace aud::spectrogram
