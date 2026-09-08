#pragma once

// LTP layer, part 1: the Heap-on-Node (HN) and the B-tree-on-Heap (BTH).
//
// An HN turns a node's data stream into a small allocator: callers hand it
// blobs and get back HIDs.  The stream is chopped into 8176-byte pieces that
// line up exactly with the data blocks NdbWriter::writeData produces, because
// an HID encodes the block index its allocation lives in.

#include <cstdint>
#include <string>
#include <vector>

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
Hid buildBth(HeapNode& hn, std::uint8_t cb_key, std::uint8_t cb_ent,
             std::vector<BthRecord> records);

}  // namespace imap2pst::pst
