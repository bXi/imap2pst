#pragma once

// A streaming heap and table context, for tables too large to hold in memory.
//
// The in-memory TableContext in ltp.h keeps every row until serialize(), which
// is fine for a message's recipients or attachments but not for a folder's
// contents: a 200k-message folder costs hundreds of megabytes and then copies
// the lot to serialize it.
//
// These two classes emit as they go.  A heap block is handed to the NDB writer
// the moment it fills, and row bytes go straight into their own data tree, so
// memory stays flat in the number of rows -- apart from the row-index records,
// which are 8 bytes each and are still accumulated (see finish()).

#include <cstdint>
#include <string>
#include <vector>

#include "pst/ltp.h"
#include "pst/ndb.h"
#include "pst/prop_tags.h"
#include "pst/pst_format.h"

namespace imap2pst::pst {

// A heap-on-node that flushes completed blocks instead of buffering them all.
//
// Block 0 is the exception: its HNHDR carries hidUserRoot, which is not known
// until the caller has built whatever the heap is holding.  It is therefore
// kept in memory and emitted last -- but its BID is placed first in the data
// tree, so the on-disk block order the reader sees is still correct.
class StreamingHeap {
 public:
    StreamingHeap(NdbWriter& ndb, std::uint8_t client_sig);

    Hid alloc(const void* data, std::size_t len);
    Hid alloc(const std::vector<std::uint8_t>& v) { return alloc(v.data(), v.size()); }

    void setUserRoot(Hid hid) { user_root_ = hid; }

    // Largest blob a heap block can hold; anything bigger becomes a subnode.
    static std::size_t maxAllocSize();

    // Flushes the remaining blocks and returns the BID of the heap's data tree.
    Bid finish();

 private:
    struct Block {
        std::vector<std::vector<std::uint8_t>> items;
        std::size_t used = 0;
    };

    static std::size_t blockHeaderSize(std::size_t block_index);
    static bool isBitmapBlock(std::size_t block_index);
    bool fits(std::size_t block_index, std::size_t len) const;
    std::vector<std::uint8_t> renderBlock(const Block& b, std::size_t block_index,
                                          bool last) const;
    void flushCurrent(bool last);

    NdbWriter& ndb_;
    std::uint8_t client_sig_;
    Hid user_root_ = 0;

    Block block0_;          // held back until finish()
    Block current_;         // the block being filled, when index > 0
    std::size_t current_index_ = 0;
    std::vector<Bid> leaves_;  // leaves_[0] is reserved for block 0
    std::uint64_t total_bytes_ = 0;
    bool finished_ = false;
};

// An append-only table context that never holds more than one row at a time.
//
// Columns are fixed at construction. Rows are written with beginRow(), a run of
// setters, then endRow().  The row matrix always lives in a subnode rather than
// being inlined in the heap when it happens to be small, because a streaming
// writer cannot know the final size up front.
class TableContextWriter {
 public:
    TableContextWriter(NdbWriter& ndb, std::vector<PropTag> columns);

    void beginRow(std::uint32_t row_id);
    void setInt16(PropTag tag, std::uint16_t v);
    void setInt32(PropTag tag, std::uint32_t v);
    void setBool(PropTag tag, bool v);
    void setInt64(PropTag tag, std::uint64_t v);
    void setTime(PropTag tag, std::uint64_t filetime) { setInt64(tag, filetime); }
    void setString(PropTag tag, const std::string& utf8);
    void setBinary(PropTag tag, const void* data, std::size_t len);
    void endRow();

    std::size_t rowCount() const { return row_count_; }

    // The node's data and subnode BIDs, ready for NdbWriter::addNode.
    struct NodeData {
        Bid data = 0;
        Bid sub = 0;
    };
    NodeData finish();

 private:
    struct Column {
        PropTag tag = 0;
        std::uint16_t ib = 0;
        std::uint8_t cb = 0;
        std::uint8_t ibit = 0;
    };

    std::size_t columnIndex(PropTag tag) const;
    void layout();
    void flushRowBlock(bool last);

    NdbWriter& ndb_;
    std::vector<PropTag> column_tags_;
    std::vector<Column> columns_;
    std::uint16_t row_size_ = 0;
    std::uint16_t rgib_[4] = {0, 0, 0, 0};
    std::size_t ceb_off_ = 0;

    StreamingHeap heap_;
    SubnodeAllocator subnodes_;

    // The row matrix, streamed a block at a time.  Rows never straddle a block
    // boundary, so a reader can compute rows-per-block from the row width.
    std::vector<std::uint8_t> row_block_;
    std::vector<Bid> row_leaves_;
    std::uint64_t row_bytes_ = 0;
    std::size_t rows_per_block_ = 0;

    std::vector<std::uint8_t> current_row_;
    bool in_row_ = false;
    std::size_t row_count_ = 0;

    // rowid -> ordinal, 8 bytes per row.  Still O(rows); see finish().
    std::vector<std::pair<std::uint32_t, std::uint32_t>> row_index_;
};

}  // namespace imap2pst::pst
