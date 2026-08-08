// Loudness (LUFS) chunk serialization.

#include "chunk_loudness.hpp"

#include "cache_manager.hpp"
#include "serialise.hpp"

namespace aud::cache::chunks {

Result<std::vector<uint8_t>> serializeLoudness(
    const aud::loudness::LoudnessResult& result
) {
    PayloadWriter writer;

    // integratedLufs (f64)
    writer.writeDouble(result.integratedLufs);

    // loudnessRangeLu (f64)
    writer.writeDouble(result.loudnessRangeLu);

    // truePeakDbtp (f64)
    writer.writeDouble(result.truePeakDbtp);

    // samplePeakDbfs (f64)
    writer.writeDouble(result.samplePeakDbfs);

    // truePeakOversampling (u32)
    writer.writeU32(result.truePeakOversampling);

    // momentaryLufs count and data
    writer.writeU32(static_cast<uint32_t>(result.momentaryLufs.size()));
    writer.writeFloatArray(std::span<const float>(result.momentaryLufs));

    // shortTermLufs count and data
    writer.writeU32(static_cast<uint32_t>(result.shortTermLufs.size()));
    writer.writeFloatArray(std::span<const float>(result.shortTermLufs));

    // usedFallbackChannelLayout (u8)
    writer.writeU8(result.usedFallbackChannelLayout ? 1 : 0);

    return writer.take();
}

Result<void> deserializeLoudness(
    std::span<const uint8_t> payload,
    aud::loudness::LoudnessResult& result
) {
    PayloadReader reader(payload);

    // integratedLufs
    auto il = reader.readDouble();
    if (!il.has_value()) return Err(il.error());
    result.integratedLufs = il.value();

    // loudnessRangeLu
    auto lr = reader.readDouble();
    if (!lr.has_value()) return Err(lr.error());
    result.loudnessRangeLu = lr.value();

    // truePeakDbtp
    auto tp = reader.readDouble();
    if (!tp.has_value()) return Err(tp.error());
    result.truePeakDbtp = tp.value();

    // samplePeakDbfs
    auto sp = reader.readDouble();
    if (!sp.has_value()) return Err(sp.error());
    result.samplePeakDbfs = sp.value();

    // truePeakOversampling
    auto tpo = reader.readU32();
    if (!tpo.has_value()) return Err(tpo.error());
    result.truePeakOversampling = tpo.value();

    // momentaryLufs
    auto mlCount = reader.readU32();
    if (!mlCount.has_value()) return Err(mlCount.error());
    auto ml = reader.readFloatArray(mlCount.value());
    if (!ml.has_value()) return Err(ml.error());
    result.momentaryLufs = ml.value();

    // shortTermLufs
    auto stCount = reader.readU32();
    if (!stCount.has_value()) return Err(stCount.error());
    auto st = reader.readFloatArray(stCount.value());
    if (!st.has_value()) return Err(st.error());
    result.shortTermLufs = st.value();

    // usedFallbackChannelLayout
    auto ufcl = reader.readU8();
    if (!ufcl.has_value()) return Err(ufcl.error());
    result.usedFallbackChannelLayout = (ufcl.value() != 0);

    return Ok();
}

uint64_t hashLoudnessConfig(const aud::loudness::LoudnessConfig& config) {
    // Hash the truePeakOversampling field
    return CacheManager::hashParameter(config.truePeakOversampling);
    // TODO: If wavChannelMask is used, include it in the hash
}

}  // namespace aud::cache::chunks
