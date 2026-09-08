#include "pst/streaming_table.h"

#include <algorithm>
#include <cstring>

namespace imap2pst::pst {
namespace {

constexpr std::size_t kHnHdrSize       = 12;
constexpr std::size_t kHnPageHdrSize   = 2;
constexpr std::size_t kHnBitmapHdrSize = 2 + 64;

std::size_t pageMapSize(std::size_t alloc_count) {
    return 4 + 2 * (alloc_count + 1);
}

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

}  // namespace

// ------------------------------------------------------------- StreamingHeap

StreamingHeap::StreamingHeap(NdbWriter& ndb, std::uint8_t client_sig)
    : ndb_(ndb), client_sig_(client_sig) {
    // Slot 0 belongs to block 0, which is emitted last but must be read first.
    leaves_.push_back(0);
}

bool StreamingHeap::isBitmapBlock(std::size_t block_index) {
    return block_index >= 8 && ((block_index - 8) % 128) == 0;
}

std::size_t StreamingHeap::blockHeaderSize(std::size_t block_index) {
    if (block_index == 0) return kHnHdrSize;
    return isBitmapBlock(block_index) ? kHnBitmapHdrSize : kHnPageHdrSize;
}

std::size_t StreamingHeap::maxAllocSize() {
    // Worst case: a bitmap block holding a single allocation.
    return kMaxBlockData - kHnBitmapHdrSize - pageMapSize(1) - 1;
}

bool StreamingHeap::fits(std::size_t block_index, std::size_t len) const {
    const Block& b = (block_index == 0) ? block0_ : current_;
    const std::size_t header = blockHeaderSize(block_index);
    // +1 covers the pad byte that may be needed to 2-byte align the page map.
    return header + b.used + len + pageMapSize(b.items.size() + 1) + 1 <= kMaxBlockData;
}

std::vector<std::uint8_t> StreamingHeap::renderBlock(const Block& b,
                                                     std::size_t block_index,
                                                     bool last) const {
    const std::size_t header = blockHeaderSize(block_index);
    const std::size_t map_size = pageMapSize(b.items.size());

    std::vector<std::uint16_t> offsets;
    offsets.reserve(b.items.size() + 1);
    std::size_t off = header;
    for (const auto& item : b.items) {
        offsets.push_back(static_cast<std::uint16_t>(off));
        off += item.size();
    }
    offsets.push_back(static_cast<std::uint16_t>(off));

    // A block that is not the last one occupies a whole NDB data block, so HID
    // block indexes stay in step with the data tree's leaves.
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
    poke16(blk.data(), static_cast<std::uint16_t>(ibHnpm));
    if (block_index == 0) {
        blk[2] = kHnSignature;
        blk[3] = client_sig_;
        poke32(blk.data() + 4, user_root_);
        std::uint8_t fill[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        const std::uint8_t code = fillLevelCode(free_bytes);
        fill[0] = static_cast<std::uint8_t>((fill[0] & 0xF0) | code);
        std::memcpy(blk.data() + 8, fill, 4);
    } else if (isBitmapBlock(block_index)) {
        std::memset(blk.data() + 2, 0xFF, 64);
    }

    for (std::size_t k = 0; k < b.items.size(); ++k) {
        if (!b.items[k].empty()) {
            std::memcpy(blk.data() + offsets[k], b.items[k].data(), b.items[k].size());
        }
    }

    std::uint8_t* pm = blk.data() + ibHnpm;
    poke16(pm, static_cast<std::uint16_t>(b.items.size()));
    // cFree is the number of freed allocation slots, not the number of
        // free bytes.  Outlook counts zero-length entries in rgibAlloc and
        // rejects the whole heap when the stored value disagrees, which makes
        // every property in the node unreadable.
        std::uint16_t freed = 0;
        for (const auto& item : b.items) {
            if (item.empty()) ++freed;
        }
        poke16(pm + 2, freed);
    for (std::size_t k = 0; k < offsets.size(); ++k) {
        poke16(pm + 4 + 2 * k, offsets[k]);
    }
    return blk;
}

void StreamingHeap::flushCurrent(bool last) {
    const auto blk = renderBlock(current_, current_index_, last);
    leaves_.push_back(ndb_.writeLeafChunk(blk.data(), blk.size()));
    total_bytes_ += blk.size();
    current_.items.clear();
    current_.used = 0;
}

Hid StreamingHeap::alloc(const void* data, std::size_t len) {
    if (len > maxAllocSize()) {
        throw PstError("heap allocation of " + std::to_string(len) +
                       " bytes exceeds the HN block capacity");
    }
    // Everything goes into block 0 until it is full, then into a rolling block.
    if (current_index_ == 0) {
        if (fits(0, len) && block0_.items.size() < 2047) {
            block0_.items.emplace_back(static_cast<const std::uint8_t*>(data),
                                       static_cast<const std::uint8_t*>(data) + len);
            block0_.used += len;
            return makeHid(0, static_cast<std::uint16_t>(block0_.items.size()));
        }
        current_index_ = 1;
    } else if (!fits(current_index_, len) || current_.items.size() >= 2047) {
        flushCurrent(/*last=*/false);
        ++current_index_;
        if (current_index_ > 0xFFFF) throw PstError("heap grew beyond 65535 blocks");
    }
    current_.items.emplace_back(static_cast<const std::uint8_t*>(data),
                               static_cast<const std::uint8_t*>(data) + len);
    current_.used += len;
    return makeHid(static_cast<std::uint16_t>(current_index_),
                   static_cast<std::uint16_t>(current_.items.size()));
}

Bid StreamingHeap::finish() {
    if (finished_) throw PstError("StreamingHeap::finish called twice");
    finished_ = true;

    const bool only_block0 = (current_index_ == 0);
    if (!only_block0) flushCurrent(/*last=*/true);

    const auto blk0 = renderBlock(block0_, 0, /*last=*/only_block0);
    leaves_[0] = ndb_.writeLeafChunk(blk0.data(), blk0.size());
    total_bytes_ += blk0.size();

    return ndb_.assembleDataTree(leaves_, total_bytes_);
}

// -------------------------------------------------------- TableContextWriter

TableContextWriter::TableContextWriter(NdbWriter& ndb, std::vector<PropTag> columns)
    : ndb_(ndb), heap_(ndb, kHnSigTC), subnodes_(ndb) {
    column_tags_.push_back(PidTagLtpRowId);
    column_tags_.push_back(PidTagLtpRowVer);
    for (PropTag tag : columns) {
        if (std::find(column_tags_.begin(), column_tags_.end(), tag) == column_tags_.end()) {
            column_tags_.push_back(tag);
        }
    }
    layout();
    rows_per_block_ = kMaxBlockData / row_size_;
    row_block_.reserve(kMaxBlockData);
}

void TableContextWriter::layout() {
    columns_.resize(column_tags_.size());
    for (std::size_t i = 0; i < column_tags_.size(); ++i) {
        const PropTag tag = column_tags_[i];
        const std::size_t fixed = fixedPropSize(tagType(tag));
        columns_[i].tag = tag;
        columns_[i].cb = static_cast<std::uint8_t>(fixed == 0 ? 4 : fixed);
        columns_[i].ibit = static_cast<std::uint8_t>(i);
    }
    // PidTagLtpRowId and PidTagLtpRowVer are pinned to offsets 0 and 4.
    std::uint16_t off = 8;
    columns_[0].ib = 0;
    columns_[1].ib = 4;
    for (std::uint8_t width : {8, 4}) {
        for (std::size_t i = 2; i < columns_.size(); ++i) {
            if (columns_[i].cb == width) { columns_[i].ib = off; off += width; }
        }
    }
    rgib_[0] = off;
    for (std::size_t i = 2; i < columns_.size(); ++i) {
        if (columns_[i].cb == 2) { columns_[i].ib = off; off += 2; }
    }
    rgib_[1] = off;
    for (std::size_t i = 2; i < columns_.size(); ++i) {
        if (columns_[i].cb == 1) { columns_[i].ib = off; off += 1; }
    }
    rgib_[2] = off;
    ceb_off_ = off;
    off = static_cast<std::uint16_t>(off + (columns_.size() + 7) / 8);
    rgib_[3] = off;
    row_size_ = off;
}

std::size_t TableContextWriter::columnIndex(PropTag tag) const {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].tag == tag) return i;
    }
    throw PstError("table column not declared: " + std::to_string(tag));
}

void TableContextWriter::beginRow(std::uint32_t row_id) {
    if (in_row_) throw PstError("beginRow called inside a row");
    in_row_ = true;
    current_row_.assign(row_size_, 0);
    row_index_.emplace_back(row_id, static_cast<std::uint32_t>(row_count_));
    setInt32(PidTagLtpRowId, row_id);
    setInt32(PidTagLtpRowVer, 1);
}

namespace {
void markPresent(std::vector<std::uint8_t>& row, std::size_t ceb_off, std::uint8_t ibit) {
    row[ceb_off + ibit / 8] |= static_cast<std::uint8_t>(0x80u >> (ibit % 8));
}
}  // namespace

void TableContextWriter::setInt16(PropTag tag, std::uint16_t v) {
    const Column& c = columns_[columnIndex(tag)];
    poke16(current_row_.data() + c.ib, v);
    markPresent(current_row_, ceb_off_, c.ibit);
}
void TableContextWriter::setInt32(PropTag tag, std::uint32_t v) {
    const Column& c = columns_[columnIndex(tag)];
    poke32(current_row_.data() + c.ib, v);
    markPresent(current_row_, ceb_off_, c.ibit);
}
void TableContextWriter::setBool(PropTag tag, bool v) {
    const Column& c = columns_[columnIndex(tag)];
    current_row_[c.ib] = v ? 1 : 0;
    markPresent(current_row_, ceb_off_, c.ibit);
}
void TableContextWriter::setInt64(PropTag tag, std::uint64_t v) {
    const Column& c = columns_[columnIndex(tag)];
    poke64(current_row_.data() + c.ib, v);
    markPresent(current_row_, ceb_off_, c.ibit);
}

void TableContextWriter::setString(PropTag tag, const std::string& utf8) {
    const std::vector<std::uint8_t> bytes =
        (tagType(tag) == kPtString8)
            ? std::vector<std::uint8_t>(utf8.begin(), utf8.end())
            : utf8ToUtf16le(utf8);
    setBinary(tag, bytes.data(), bytes.size());
}

void TableContextWriter::setBinary(PropTag tag, const void* data, std::size_t len) {
    const Column& c = columns_[columnIndex(tag)];
    std::uint32_t hnid = 0;
    if (len == 0) {
        hnid = 0;  // an empty variable-length value is encoded as HNID 0
    } else if (len <= StreamingHeap::maxAllocSize()) {
        hnid = heap_.alloc(data, len);
    } else {
        hnid = subnodes_.spill(data, len);
    }
    poke32(current_row_.data() + c.ib, hnid);
    markPresent(current_row_, ceb_off_, c.ibit);
}

void TableContextWriter::flushRowBlock(bool last) {
    if (row_block_.empty()) return;
    if (!last) row_block_.resize(kMaxBlockData, 0);
    row_leaves_.push_back(ndb_.writeLeafChunk(row_block_.data(), row_block_.size()));
    row_bytes_ += row_block_.size();
    row_block_.clear();
}

void TableContextWriter::endRow() {
    if (!in_row_) throw PstError("endRow without beginRow");
    in_row_ = false;
    if (row_block_.size() + row_size_ > rows_per_block_ * row_size_) {
        flushRowBlock(/*last=*/false);
    }
    row_block_.insert(row_block_.end(), current_row_.begin(), current_row_.end());
    ++row_count_;
}

TableContextWriter::NodeData TableContextWriter::finish() {
    flushRowBlock(/*last=*/true);

    std::uint32_t hnid_rows = 0;
    if (!row_leaves_.empty()) {
        const Bid rows_bid = ndb_.assembleDataTree(row_leaves_, row_bytes_);
        hnid_rows = subnodes_.adopt(rows_bid);
    }

    // Row index: rowid -> ordinal.  Keys arrive in ascending order for the
    // tables this writer serves, but sort anyway so callers are not constrained.
    std::sort(row_index_.begin(), row_index_.end());
    std::vector<BthRecord> index;
    index.reserve(row_index_.size());
    for (const auto& entry : row_index_) {
        BthRecord r;
        put32(r.key, entry.first);
        put32(r.value, entry.second);
        index.push_back(std::move(r));
    }
    row_index_.clear();
    row_index_.shrink_to_fit();
    const Hid hid_row_index = buildBthInto(heap_, 4, 4, std::move(index));

    std::vector<Column> sorted = columns_;
    std::sort(sorted.begin(), sorted.end(),
              [](const Column& a, const Column& b) { return a.tag < b.tag; });

    std::vector<std::uint8_t> info;
    put8(info, kHnSigTC);
    put8(info, static_cast<std::uint8_t>(columns_.size()));
    for (int i = 0; i < 4; ++i) put16(info, rgib_[i]);
    put32(info, hid_row_index);
    put32(info, hnid_rows);
    put32(info, 0);  // hidIndex, deprecated
    for (const auto& c : sorted) {
        put32(info, c.tag);
        put16(info, c.ib);
        put8(info, c.cb);
        put8(info, c.ibit);
    }

    heap_.setUserRoot(heap_.alloc(info));
    NodeData out;
    out.data = heap_.finish();
    out.sub = ndb_.writeSubnodes(subnodes_.entries());
    return out;
}

}  // namespace imap2pst::pst
