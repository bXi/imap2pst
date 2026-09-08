#pragma once

#include <cstddef>
#include <cstdint>

namespace imap2pst::pst {

// [MS-PST] 5.3.  Reflected CRC-32 over polynomial 0xEDB88320, but -- unlike
// zlib's crc32 -- with no pre-inversion of the seed and no final inversion.
// libpff calls the same function "weak CRC-32".
std::uint32_t computeCrc(std::uint32_t seed, const void* data, std::size_t len);

inline std::uint32_t computeCrc(const void* data, std::size_t len) {
    return computeCrc(0, data, len);
}

}  // namespace imap2pst::pst
