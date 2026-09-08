#include "pst/crc.h"

#include <array>

namespace imap2pst::pst {
namespace {

constexpr std::array<std::uint32_t, 256> makeTable() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        t[i] = c;
    }
    return t;
}

constexpr auto kTable = makeTable();

}  // namespace

std::uint32_t computeCrc(std::uint32_t seed, const void* data, std::size_t len) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = seed;
    for (std::size_t i = 0; i < len; ++i) {
        crc = kTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

}  // namespace imap2pst::pst
