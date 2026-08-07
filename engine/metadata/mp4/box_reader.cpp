#include "box_reader.hpp"

namespace aud::metadata::mp4 {

namespace {

std::uint32_t readU32Be(std::span<const std::byte> b, std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(b[offset]) << 24) | (static_cast<std::uint32_t>(b[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(b[offset + 2]) << 8) | static_cast<std::uint32_t>(b[offset + 3]);
}

std::uint64_t readU64Be(std::span<const std::byte> b, std::size_t offset) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<std::uint8_t>(b[offset + i]);
    return v;
}

}  // namespace

std::size_t forEachBox(std::span<const std::byte> bytes, const std::function<void(const Box&)>& visit) {
    std::size_t cursor = 0;
    std::size_t visited = 0;

    while (cursor + 8 <= bytes.size()) {
        const std::uint32_t declaredSize = readU32Be(bytes, cursor);
        const std::string    type(reinterpret_cast<const char*>(bytes.data() + cursor + 4), 4);

        std::size_t headerLen;
        std::uint64_t totalSize;
        if (declaredSize == 1) {
            if (cursor + 16 > bytes.size()) break;  // largesize field itself doesn't fit — stop
            totalSize = readU64Be(bytes, cursor + 8);
            headerLen  = 16;
        } else if (declaredSize == 0) {
            totalSize = bytes.size() - cursor;  // "extends to end of data"
            headerLen  = 8;
        } else {
            totalSize = declaredSize;
            headerLen  = 8;
        }

        if (totalSize < headerLen || cursor + totalSize > bytes.size()) break;  // corrupt/truncated — stop safely

        const std::size_t bodyLen = static_cast<std::size_t>(totalSize) - headerLen;
        Box                 box{type, bytes.subspan(cursor + headerLen, bodyLen)};
        visit(box);
        ++visited;

        cursor += static_cast<std::size_t>(totalSize);
    }

    return visited;
}

}  // namespace aud::metadata::mp4
