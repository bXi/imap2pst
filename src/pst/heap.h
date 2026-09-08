#pragma once

// LTP layer, part 1: the Heap-on-Node (HN) and the B-tree-on-Heap (BTH).
//
// An HN turns a node's data stream into a small allocator: callers hand it
// blobs and get back HIDs.  The stream is chopped into 8176-byte pieces that
// line up exactly with the data blocks NdbWriter::writeData produces, because
// an HID encodes the block index its allocation lives in.

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "pst/ndb.h"
#include "pst/pst_format.h"

namespace imap2pst::pst {

class HeapNode {
 public:
    explicit HeapNode(std::uint8_t client_sig) : client_sig_(client_sig) {
        blocks_.emplace_back();
    }

    // Copies `len` bytes into the heap and returns its HID.  A zero-length
    // allocation is legal and is how empty strings are represented.
    Hid alloc(const void* data, std::size_t len);
    Hid alloc(const std::vector<std::uint8_t>& v) { return alloc(v.data(), v.size()); }

    // Rewrites an existing allocation.  The new content must be the same size,
    // which is all the two-pass table-context serializer needs.
    void patch(Hid hid, const void* data, std::size_t len);

    void setUserRoot(Hid hid) { user_root_ = hid; }

    // Largest blob that can be stored in a heap block.  Anything bigger has to
    // become a subnode instead.
    static std::size_t maxAllocSize();

    // The full HN byte stream, ready to hand to NdbWriter::writeData.
    std::vector<std::uint8_t> serialize() const;

 private:
    struct Block {
        std::vector<std::vector<std::uint8_t>> items;
        std::size_t used = 0;  // bytes consumed by items
    };

    // Bytes of per-block header: HNHDR, HNPAGEHDR or HNBITMAPHDR.
    static std::size_t blockHeaderSize(std::size_t block_index);
    static bool isBitmapBlock(std::size_t block_index);
    bool fits(std::size_t block_index, std::size_t len) const;

    std::uint8_t client_sig_;
    Hid user_root_ = 0;
    std::vector<Block> blocks_;
};

// ------------------------------------------------------------------- BTH

struct BthRecord {
    std::vector<std::uint8_t> key;    // exactly cb_key bytes, little endian
    std::vector<std::uint8_t> value;  // exactly cb_ent bytes
};

// Serializes `records` as a BTH inside `hn` and returns the HID of its
// BTHHEADER.  Records are sorted by key here, so callers need not pre-sort.
//
// Templated on the heap so it serves both HeapNode and the StreamingHeap in
// streaming_table.h; any type with alloc() and a static maxAllocSize() works.
template <typename Heap>
Hid buildBthInto(Heap& hn, std::uint8_t cb_key, std::uint8_t cb_ent,
                 std::vector<BthRecord> records) {
    std::sort(records.begin(), records.end(),
              [](const BthRecord& a, const BthRecord& b) {
                  auto value = [](const std::vector<std::uint8_t>& k) {
                      std::uint64_t v = 0;
                      for (std::size_t i = k.size(); i-- > 0;) v = (v << 8) | k[i];
                      return v;
                  };
                  return value(a.key) < value(b.key);
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
    const std::size_t per_leaf = std::max<std::size_t>(1, Heap::maxAllocSize() / leaf_rec);

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
    const std::size_t per_inter = std::max<std::size_t>(1, Heap::maxAllocSize() / inter_rec);
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

inline Hid buildBth(HeapNode& hn, std::uint8_t cb_key, std::uint8_t cb_ent,
                    std::vector<BthRecord> records) {
    return buildBthInto(hn, cb_key, cb_ent, std::move(records));
}

}  // namespace imap2pst::pst
