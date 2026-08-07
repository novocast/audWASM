#pragma once

// Bin -> display-row frequency mapping (M07 "Frequency axis"). Tile rows are already in display
// frequency space, not linear FFT bins: users almost always want a log/mel axis (linear wastes most
// of the display on the top two octaves), and precomputing the mapping once per config means the
// expensive part (band aggregation) happens once per tile generation, not once per rendered frame.
//
// Decision — low rows (narrower than one FFT bin) interpolate between two neighbouring bins; high
// rows (spanning many bins) aggregate by MAX, never mean. Averaging away a narrow peak in the top
// octave would make the spectrogram lie about a transient/harmonic that's actually there.

#include <cstddef>
#include <span>
#include <vector>

#include "../util/audio_types.hpp"
#include "../util/result.hpp"
#include "tile.hpp"

namespace aud::spectrogram {

// One output row's aggregation recipe. `binLo`/`binHi` are always populated (a single-bin
// interpolation is `binHi == binLo + 1`); `interpolate` selects which reducer applies:
//   interpolate: lerp(magnitude[binLo], magnitude[binLo+1], frac)
//   !interpolate: max(magnitude[binLo .. binHi))
struct FreqRow {
    bool          interpolate = true;
    std::uint32_t binLo       = 0;
    std::uint32_t binHi       = 0;  // exclusive; only meaningful when !interpolate
    float         frac        = 0.0f;
    double        centerHz    = 0.0;  // for the frequency ruler / point-query row lookup
};

class FreqMapping {
public:
    // `fftBinCount` is fftSize/2+1 (StftProcessor::binCount()/FrameComputer::binCount()).
    // `outputRows` defaults to kTileHeight; exposed as a parameter so the overview strip (which
    // uses the same row count) and any future non-tile consumer can reuse this.
    static Result<FreqMapping> create(FreqAxis axis, double minHz, double sampleRate,
                                       std::size_t fftBinCount, std::size_t outputRows = kTileHeight);

    [[nodiscard]] std::size_t      rowCount() const noexcept { return m_rows.size(); }
    [[nodiscard]] const FreqRow&   row(std::size_t rowIndex) const noexcept { return m_rows[rowIndex]; }

    // Reduces `magnitudes` (fftBinCount linear amplitudes, DC..Nyquist) into `outDb` (rowCount()
    // entries, row 0 = lowest frequency), applying each row's recipe then converting to dB.
    void mapToDb(std::span<const float> magnitudes, std::span<double> outDb) const noexcept;

    // Nearest row to a target frequency — used by the cursor readout / point query to know which
    // row (and therefore which aggregation recipe) the user is hovering over.
    [[nodiscard]] std::size_t nearestRow(double hz) const noexcept;

private:
    std::vector<FreqRow> m_rows;
};

}  // namespace aud::spectrogram
