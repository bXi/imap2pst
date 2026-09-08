#include "pst/heap.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "pst/ndb.h"

namespace imap2pst::pst {
namespace {

constexpr std::size_t kHnHdrSize       = 12;
constexpr std::size_t kHnPageHdrSize   = 2;
constexpr std::size_t kHnBitmapHdrSize = 2 + 64;

// Fill-level codes, [MS-PST] 2.3.1.2.  Purely advisory for readers.
std::uint8_t fillLevelCode(std::size_t free_bytes) {
    if (free_bytes >= 3584) return 0x0;
    if (free_bytes >= 2560) return 0x1;
    if (free_bytes >= 2048) return 0x2;
    if (free_bytes >= 1792) return 0x3;
    if (free_bytes >= 1536) return 0x4;
    if (free_bytes >= 1280) return 0x5;
    if (free_bytes >= 1024) return 0x6;
    if (free_bytes >= 768)  return 0x7;
    if (free_bytes >= 512)  return 0x8;
    if (free_bytes >= 256)  return 0x9;
    return 0xF;
}

std::size_t pageMapSize(std::size_t alloc_count) {
    return 4 + 2 * (alloc_count + 1);
}

}  // namespace

bool HeapNode::isBitmapBlock(std::size_t block_index) {
    // Blocks 8, 136, 264, ... carry an HNBITMAPHDR instead of an HNPAGEHDR.
    return block_index >= 8 && ((block_index - 8) % 128) == 0;
}

std::size_t HeapNode::blockHeaderSize(std::size_t block_index) {
    if (block_index == 0) return kHnHdrSize;
    return isBitmapBlock(block_index) ? kHnBitmapHdrSize : kHnPageHdrSize;
}

std::size_t HeapNode::maxAllocSize() {
    // Worst case: a bitmap block holding a single allocation.
    return kMaxBlockData - kHnBitmapHdrSize - pageMapSize(1) - 1;
}

bool HeapNode::fits(std::size_t block_index, std::size_t len) const {
    const Block& b = blocks_[block_index];
    const std::size_t header = blockHeaderSize(block_index);
    // +1 covers the pad byte that may be needed to 2-byte align the page map.
    const std::size_t needed =
        header + b.used + len + pageMapSize(b.items.size() + 1) + 1;
    return needed <= kMaxBlockData;
}

Hid HeapNode::alloc(const void* data, std::size_t len) {
    if (len > maxAllocSize()) {
        throw PstError("heap allocation of " + std::to_string(len) +
                       " bytes exceeds the HN block capacity");
    }
    std::size_t index = blocks_.size() - 1;
    // An HID's allocation index is 11 bits and is 1-based.
    if (!fits(index, len) || blocks_[index].items.size() >= 2047) {
        blocks_.emplace_back();
        index = blocks_.size() - 1;
        if (index > 0xFFFF) throw PstError("heap grew beyond 65535 blocks");
    }
    Block& b = blocks_[index];
    b.items.emplace_back(static_cast<const std::uint8_t*>(data),
                         static_cast<const std::uint8_t*>(data) + len);
    b.used += len;
    return makeHid(static_cast<std::uint16_t>(index),
                   static_cast<std::uint16_t>(b.items.size()));
}

void HeapNode::patch(Hid hid, const void* data, std::size_t len) {
    const std::size_t block = hidBlockIndex(hid);
    const std::size_t index = hidAllocIndex(hid);
    if (block >= blocks_.size() || index == 0 || index > blocks_[block].items.size()) {
        throw PstError("patch of an unknown HID");
    }
    auto& item = blocks_[block].items[index - 1];
    if (item.size() != len) throw PstError("patch would change an allocation's size");
    std::memcpy(item.data(), data, len);
}

std::vector<std::uint8_t> HeapNode::serialize() const {
    std::vector<std::uint8_t> out;
    const std::size_t nblocks = blocks_.size();

    for (std::size_t i = 0; i < nblocks; ++i) {
        const Block& b = blocks_[i];
        const std::size_t header = blockHeaderSize(i);
        const std::size_t map_size = pageMapSize(b.items.size());
        const bool last = (i + 1 == nblocks);

        // Offsets of each allocation, relative to the start of the block.
        std::vector<std::uint16_t> offsets;
        offsets.reserve(b.items.size() + 1);
        std::size_t off = header;
        for (const auto& item : b.items) {
            offsets.push_back(static_cast<std::uint16_t>(off));
            off += item.size();
        }
        offsets.push_back(static_cast<std::uint16_t>(off));

        // The page map must start on a 2-byte boundary.  In a block that is not
        // the last one we push it to the very end so the block occupies exactly
        // one NDB data block and HID block indexes stay in step.
        std::size_t ibHnpm = alignUp(off, 2);
        std::size_t block_size;
        if (last) {
            block_size = ibHnpm + map_size;
        } else {
            block_size = kMaxBlockData;
            ibHnpm = block_size - map_size;
        }
        const std::size_t free_bytes = ibHnpm - off;

        std::vector<std::uint8_t> blk(block_size, 0);

        if (i == 0) {
            poke16(blk.data() + 0, static_cast<std::uint16_t>(ibHnpm));
            blk[2] = kHnSignature;
            blk[3] = client_sig_;
            poke32(blk.data() + 4, user_root_);
            // rgbFillLevel: one nibble per block for blocks 0..7, low nibble
            // first.  0xF ("full") for blocks that do not exist.
            std::uint8_t fill[4] = {0xFF, 0xFF, 0xFF, 0xFF};
            for (std::size_t k = 0; k < 8; ++k) {
                std::uint8_t code = 0xF;
                if (k < nblocks) {
                    code = (k == 0) ? fillLevelCode(free_bytes) : 0x0;
                }
                if (k % 2 == 0) {
                    fill[k / 2] = static_cast<std::uint8_t>((fill[k / 2] & 0xF0) | code);
                } else {
                    fill[k / 2] =
                        static_cast<std::uint8_t>((fill[k / 2] & 0x0F) | (code << 4));
                }
            }
            std::memcpy(blk.data() + 8, fill, 4);
        } else if (isBitmapBlock(i)) {
            poke16(blk.data() + 0, static_cast<std::uint16_t>(ibHnpm));
            std::memset(blk.data() + 2, 0xFF, 64);
        } else {
            poke16(blk.data() + 0, static_cast<std::uint16_t>(ibHnpm));
        }

        for (std::size_t k = 0; k < b.items.size(); ++k) {
            if (!b.items[k].empty()) {
                std::memcpy(blk.data() + offsets[k], b.items[k].data(), b.items[k].size());
            }
        }

        std::uint8_t* pm = blk.data() + ibHnpm;
        poke16(pm, static_cast<std::uint16_t>(b.items.size()));       // cAlloc
        poke16(pm + 2, static_cast<std::uint16_t>(free_bytes));       // cFree
        for (std::size_t k = 0; k < offsets.size(); ++k) {
            poke16(pm + 4 + 2 * k, offsets[k]);
        }

        out.insert(out.end(), blk.begin(), blk.end());
    }
    return out;
}

// ------------------------------------------------------------------- BTH

namespace {

std::uint64_t keyValue(const std::vector<std::uint8_t>& key) {
    std::uint64_t v = 0;
    for (std::size_t i = key.size(); i-- > 0;) v = (v << 8) | key[i];
    return v;
}

}  // namespace

Hid buildBth(HeapNode& hn, std::uint8_t cb_key, std::uint8_t cb_ent,
             std::vector<BthRecord> records) {
    std::sort(records.begin(), records.end(),
              [](const BthRecord& a, const BthRecord& b) {
                  return keyValue(a.key) < keyValue(b.key);
              });

    std::vector<std::uint8_t> header;
    put8(header, kHnSigBTH);
    put8(header, cb_key);
    put8(header, cb_ent);

    if (records.empty()) {
        put8(header, 0);   // bIdxLevels
        put32(header, 0);  // hidRoot: none
        return hn.alloc(header);
    }

    const std::size_t leaf_rec = static_cast<std::size_t>(cb_key) + cb_ent;
    const std::size_t per_leaf = std::max<std::size_t>(1, HeapNode::maxAllocSize() / leaf_rec);

    struct Level { std::vector<std::uint8_t> key; Hid hid; };
    std::vector<Level> level;
    for (std::size_t i = 0; i < records.size(); i += per_leaf) {
        const std::size_t n = std::min(per_leaf, records.size() - i);
        std::vector<std::uint8_t> page;
        page.reserve(n * leaf_rec);
        for (std::size_t k = i; k < i + n; ++k) {
            putBytes(page, records[k].key.data(), cb_key);
            putBytes(page, records[k].value.data(), cb_ent);
        }
        level.push_back({records[i].key, hn.alloc(page)});
    }

    std::uint8_t levels = 0;
    const std::size_t inter_rec = static_cast<std::size_t>(cb_key) + 4;
    const std::size_t per_inter = std::max<std::size_t>(1, HeapNode::maxAllocSize() / inter_rec);
    while (level.size() > 1) {
        std::vector<Level> next;
        for (std::size_t i = 0; i < level.size(); i += per_inter) {
            const std::size_t n = std::min(per_inter, level.size() - i);
            std::vector<std::uint8_t> page;
            page.reserve(n * inter_rec);
            for (std::size_t k = i; k < i + n; ++k) {
                putBytes(page, level[k].key.data(), cb_key);
                put32(page, level[k].hid);
            }
            next.push_back({level[i].key, hn.alloc(page)});
        }
        level = std::move(next);
        ++levels;
        if (levels > 8) throw PstError("BTH grew beyond 8 index levels");
    }

    put8(header, levels);
    put32(header, level.front().hid);
    return hn.alloc(header);
}

}  // namespace imap2pst::pst
