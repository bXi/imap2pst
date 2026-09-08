#include "pst/ltp.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace imap2pst::pst {

// --------------------------------------------------------------- format bits

std::vector<std::uint8_t> utf8ToUtf16le(const std::string& s) {
    std::vector<std::uint8_t> out;
    out.reserve(s.size() * 2);
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<std::uint8_t>(s[i]);
        std::uint32_t cp;
        std::size_t extra;
        if (c < 0x80) {
            cp = c; extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1Fu; extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0Fu; extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07u; extra = 3;
        } else {
            cp = 0xFFFD; extra = 0;  // invalid lead byte
        }
        if (i + extra >= s.size()) { cp = 0xFFFD; extra = 0; }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cc = static_cast<std::uint8_t>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { cp = 0xFFFD; extra = 0; break; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        i += extra + 1;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp < 0x10000) {
            put16(out, static_cast<std::uint16_t>(cp));
        } else {
            cp -= 0x10000;
            put16(out, static_cast<std::uint16_t>(0xD800 + (cp >> 10)));
            put16(out, static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF)));
        }
    }
    return out;
}

std::uint64_t unixToFiletime(std::int64_t unix_seconds) {
    // 11644473600 seconds between 1601-01-01 and 1970-01-01.
    if (unix_seconds <= -11644473600LL) return 0;
    return static_cast<std::uint64_t>(unix_seconds + 11644473600LL) * 10000000ULL;
}

// ------------------------------------------------------------ SubnodeAllocator

Nid SubnodeAllocator::spill(const void* data, std::size_t len) {
    return adopt(ndb_.writeData(data, len));
}

Nid SubnodeAllocator::adopt(Bid data) {
    const Nid nid = makeNid(kNidTypeLtp, next_index_++);
    entries_.push_back({nid, data, 0});
    return nid;
}

// ------------------------------------------------------------ PropertyContext

void PropertyContext::put(PropTag tag, Value v) {
    v.tag = tag;
    for (auto& existing : values_) {
        if (tagId(existing.tag) == tagId(tag)) {
            existing = v;
            return;
        }
    }
    values_.push_back(std::move(v));
}

void PropertyContext::setInt16(PropTag tag, std::uint16_t v) {
    put(tag, Value{tag, Value::Kind::kInline, v, {}});
}
void PropertyContext::setInt32(PropTag tag, std::uint32_t v) {
    put(tag, Value{tag, Value::Kind::kInline, v, {}});
}
void PropertyContext::setBool(PropTag tag, bool v) {
    put(tag, Value{tag, Value::Kind::kInline, v ? 1u : 0u, {}});
}
void PropertyContext::setSpilledValue(PropTag tag, Nid nid) {
    put(tag, Value{tag, Value::Kind::kSpilled, nid, {}});
}
void PropertyContext::setInt64(PropTag tag, std::uint64_t v) {
    std::vector<std::uint8_t> b;
    put64(b, v);
    put(tag, Value{tag, Value::Kind::kBytes, 0, std::move(b)});
}
void PropertyContext::setTime(PropTag tag, std::uint64_t filetime) {
    setInt64(tag, filetime);
}
void PropertyContext::setString(PropTag tag, const std::string& utf8) {
    if (tagType(tag) == kPtString8) {
        put(tag, Value{tag, Value::Kind::kBytes, 0,
                       std::vector<std::uint8_t>(utf8.begin(), utf8.end())});
    } else {
        put(tag, Value{tag, Value::Kind::kBytes, 0, utf8ToUtf16le(utf8)});
    }
}
void PropertyContext::setBinary(PropTag tag, const void* data, std::size_t len) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    put(tag, Value{tag, Value::Kind::kBytes, 0, std::vector<std::uint8_t>(p, p + len)});
}

bool PropertyContext::has(PropTag tag) const {
    return std::any_of(values_.begin(), values_.end(),
                       [&](const Value& v) { return tagId(v.tag) == tagId(tag); });
}

std::vector<std::uint8_t> PropertyContext::serialize(SubnodeAllocator& subs) const {
    HeapNode hn(kHnSigPC);

    std::vector<BthRecord> records;
    records.reserve(values_.size());
    for (const auto& v : values_) {
        std::uint32_t hnid;
        if (v.kind == Value::Kind::kInline || v.kind == Value::Kind::kSpilled) {
            hnid = v.hnid;
        } else if (v.bytes.size() <= HeapNode::maxAllocSize()) {
            hnid = hn.alloc(v.bytes);
        } else {
            hnid = subs.spill(v.bytes.data(), v.bytes.size());
        }
        BthRecord r;
        put16(r.key, tagId(v.tag));
        put16(r.value, static_cast<std::uint16_t>(tagType(v.tag)));
        put32(r.value, hnid);
        records.push_back(std::move(r));
    }

    hn.setUserRoot(buildBth(hn, 2, 6, std::move(records)));
    return hn.serialize();
}

// --------------------------------------------------------------- TableContext

TableContext::TableContext() {
    columns_.push_back(PidTagLtpRowId);
    columns_.push_back(PidTagLtpRowVer);
}

void TableContext::addColumn(PropTag tag) {
    if (std::find(columns_.begin(), columns_.end(), tag) != columns_.end()) return;
    columns_.push_back(tag);
    for (auto& row : rows_) row.cells.resize(columns_.size());
}

std::size_t TableContext::columnIndex(PropTag tag) const {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i] == tag) return i;
    }
    throw PstError("table column not declared: " + std::to_string(tag));
}

std::size_t TableContext::addRow(std::uint32_t row_id) {
    Row r;
    r.row_id = row_id;
    r.cells.resize(columns_.size());
    rows_.push_back(std::move(r));
    const std::size_t idx = rows_.size() - 1;
    setInt32(idx, PidTagLtpRowId, row_id);
    setInt32(idx, PidTagLtpRowVer, 1);
    return idx;
}

TableContext::Cell& TableContext::cell(std::size_t row, PropTag tag) {
    if (row >= rows_.size()) throw PstError("table row index out of range");
    return rows_[row].cells[columnIndex(tag)];
}

void TableContext::setInt16(std::size_t row, PropTag tag, std::uint16_t v) {
    Cell& c = cell(row, tag);
    c.present = true; c.variable = false; c.bytes.clear();
    put16(c.bytes, v);
}
void TableContext::setInt32(std::size_t row, PropTag tag, std::uint32_t v) {
    Cell& c = cell(row, tag);
    c.present = true; c.variable = false; c.bytes.clear();
    put32(c.bytes, v);
}
void TableContext::setBool(std::size_t row, PropTag tag, bool v) {
    Cell& c = cell(row, tag);
    c.present = true; c.variable = false; c.bytes.clear();
    put8(c.bytes, v ? 1 : 0);
}
void TableContext::setInt64(std::size_t row, PropTag tag, std::uint64_t v) {
    Cell& c = cell(row, tag);
    c.present = true; c.variable = false; c.bytes.clear();
    put64(c.bytes, v);
}
void TableContext::setTime(std::size_t row, PropTag tag, std::uint64_t filetime) {
    setInt64(row, tag, filetime);
}
void TableContext::setString(std::size_t row, PropTag tag, const std::string& utf8) {
    Cell& c = cell(row, tag);
    c.present = true; c.variable = true;
    c.bytes = (tagType(tag) == kPtString8)
                  ? std::vector<std::uint8_t>(utf8.begin(), utf8.end())
                  : utf8ToUtf16le(utf8);
}
void TableContext::setBinary(std::size_t row, PropTag tag, const void* data,
                             std::size_t len) {
    Cell& c = cell(row, tag);
    c.present = true; c.variable = true;
    const auto* p = static_cast<const std::uint8_t*>(data);
    c.bytes.assign(p, p + len);
}

std::vector<TableContext::Column> TableContext::layout(std::uint16_t* row_size,
                                                       std::uint16_t rgib[4]) const {
    std::vector<Column> cols(columns_.size());
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const PropTag tag = columns_[i];
        const std::size_t fixed = fixedPropSize(tagType(tag));
        // Variable-width values are addressed by a 4-byte HNID.
        cols[i].tag = tag;
        cols[i].cb = static_cast<std::uint8_t>(fixed == 0 ? 4 : fixed);
        cols[i].ibit = static_cast<std::uint8_t>(i);
    }

    // PidTagLtpRowId and PidTagLtpRowVer are pinned to offsets 0 and 4; the
    // remaining columns are packed widest-first so every value stays aligned.
    std::uint16_t off = 8;
    cols[0].ib = 0;
    cols[1].ib = 4;
    for (std::uint8_t width : {8, 4}) {
        for (std::size_t i = 2; i < cols.size(); ++i) {
            if (cols[i].cb == width) { cols[i].ib = off; off += width; }
        }
    }
    rgib[0] = off;  // TCI_4b: end of the 4- and 8-byte group
    for (std::size_t i = 2; i < cols.size(); ++i) {
        if (cols[i].cb == 2) { cols[i].ib = off; off += 2; }
    }
    rgib[1] = off;  // TCI_2b
    for (std::size_t i = 2; i < cols.size(); ++i) {
        if (cols[i].cb == 1) { cols[i].ib = off; off += 1; }
    }
    rgib[2] = off;  // TCI_1b: start of the cell-existence bitmap
    off = static_cast<std::uint16_t>(off + (cols.size() + 7) / 8);
    rgib[3] = off;  // TCI_bm: total row width
    *row_size = off;
    return cols;
}

std::vector<std::uint8_t> TableContext::serialize(SubnodeAllocator& subs) const {
    HeapNode hn(kHnSigTC);

    std::uint16_t row_size = 0;
    std::uint16_t rgib[4] = {0, 0, 0, 0};
    std::vector<Column> cols = layout(&row_size, rgib);
    const std::size_t ceb_off = rgib[2];
    const std::size_t ceb_len = (cols.size() + 7) / 8;

    // Build the row matrix.  Variable-width cells become HNIDs, which means
    // their heap allocations have to happen before the matrix itself.
    std::vector<std::uint8_t> matrix;
    matrix.reserve(rows_.size() * row_size);
    for (const auto& row : rows_) {
        std::vector<std::uint8_t> buf(row_size, 0);
        for (std::size_t i = 0; i < cols.size(); ++i) {
            const Cell& c = row.cells[i];
            if (!c.present) continue;
            if (c.variable) {
                std::uint32_t hnid = 0;
                if (c.bytes.empty()) {
                    // An empty variable-length value is encoded as HNID 0.
                    hnid = 0;
                } else if (c.bytes.size() <= HeapNode::maxAllocSize()) {
                    hnid = hn.alloc(c.bytes);
                } else {
                    hnid = subs.spill(c.bytes.data(), c.bytes.size());
                }
                poke32(buf.data() + cols[i].ib, hnid);
            } else {
                std::memcpy(buf.data() + cols[i].ib, c.bytes.data(),
                            std::min<std::size_t>(cols[i].cb, c.bytes.size()));
            }
            buf[ceb_off + cols[i].ibit / 8] |=
                static_cast<std::uint8_t>(0x80u >> (cols[i].ibit % 8));
        }
        matrix.insert(matrix.end(), buf.begin(), buf.end());
    }

    // Row storage: inline in the heap when it fits, otherwise a subnode whose
    // blocks each hold a whole number of rows.
    std::uint32_t hnid_rows = 0;
    if (!rows_.empty()) {
        if (matrix.size() <= HeapNode::maxAllocSize()) {
            hnid_rows = hn.alloc(matrix);
        } else {
            const std::size_t rows_per_block = kMaxBlockData / row_size;
            std::vector<std::uint8_t> padded;
            padded.reserve(matrix.size() + kMaxBlockData);
            for (std::size_t i = 0; i < rows_.size(); i += rows_per_block) {
                const std::size_t n = std::min(rows_per_block, rows_.size() - i);
                const auto* p = matrix.data() + i * row_size;
                padded.insert(padded.end(), p, p + n * row_size);
                if (i + n < rows_.size()) padded.resize(padded.size() + (kMaxBlockData - n * row_size));
            }
            hnid_rows = subs.spill(padded.data(), padded.size());
        }
    }

    // Row index: row id -> ordinal position in the matrix.
    std::vector<BthRecord> index;
    index.reserve(rows_.size());
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        BthRecord r;
        put32(r.key, rows_[i].row_id);
        put32(r.value, static_cast<std::uint32_t>(i));
        index.push_back(std::move(r));
    }
    const Hid hid_row_index = buildBth(hn, 4, 4, std::move(index));

    // TCINFO, [MS-PST] 2.3.4.1.  rgTCOLDESC is emitted in tag order.
    std::vector<Column> sorted = cols;
    std::sort(sorted.begin(), sorted.end(),
              [](const Column& a, const Column& b) { return a.tag < b.tag; });

    std::vector<std::uint8_t> info;
    put8(info, kHnSigTC);
    put8(info, static_cast<std::uint8_t>(cols.size()));
    for (int i = 0; i < 4; ++i) put16(info, rgib[i]);
    put32(info, hid_row_index);
    put32(info, hnid_rows);
    put32(info, 0);  // hidIndex, deprecated
    for (const auto& c : sorted) {
        put32(info, c.tag);
        put16(info, c.ib);
        put8(info, c.cb);
        put8(info, c.ibit);
    }

    hn.setUserRoot(hn.alloc(info));
    return hn.serialize();
}

}  // namespace imap2pst::pst
