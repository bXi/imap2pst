#pragma once

// LTP layer, part 2: property contexts and table contexts.
//
// Both are heap-on-node structures.  A PC is a BTH keyed by property id whose
// values are either stored inline (<= 4 bytes) or referenced by HNID; a TC adds
// a row matrix and a row-index BTH on top of the same idea.
//
// Values too large for a heap block are spilled into subnodes of the owning
// node, which is what SubnodeAllocator hands out.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "pst/heap.h"
#include "pst/ndb.h"
#include "pst/prop_tags.h"

namespace imap2pst::pst {

// Hands out subnode NIDs for one node being built, and writes their data trees
// through the NDB writer as it goes.
class SubnodeAllocator {
 public:
    explicit SubnodeAllocator(NdbWriter& ndb) : ndb_(ndb) {}

    // Spills a blob into a fresh subnode and returns the NID to reference it by.
    Nid spill(const void* data, std::size_t len);

    // Registers a subnode built elsewhere (a nested PC or TC).
    void add(Nid nid, Bid data, Bid sub) { entries_.push_back({nid, data, sub}); }

    // Takes a data tree the caller has already written and gives it a NID.
    // Streaming writers need this: they emit their blocks as they go and only
    // have a root BID at the end, so there is nothing left to spill().
    Nid adopt(Bid data);

    const std::vector<SubnodeEntry>& entries() const { return entries_; }
    NdbWriter& ndb() { return ndb_; }

 private:
    NdbWriter& ndb_;
    std::vector<SubnodeEntry> entries_;
    std::uint32_t next_index_ = 1;
};

// ------------------------------------------------------------ PropertyContext

class PropertyContext {
 public:
    void setInt16(PropTag tag, std::uint16_t v);
    void setInt32(PropTag tag, std::uint32_t v);
    void setBool(PropTag tag, bool v);
    void setInt64(PropTag tag, std::uint64_t v);
    // `filetime` is 100ns ticks since 1601-01-01.
    void setTime(PropTag tag, std::uint64_t filetime);
    void setString(PropTag tag, const std::string& utf8);
    void setBinary(PropTag tag, const void* data, std::size_t len);
    void setBinary(PropTag tag, const std::vector<std::uint8_t>& v) {
        setBinary(tag, v.data(), v.size());
    }
    // PT_MV_UNICODE: a count, an offset per item, then the items themselves.
    void setStringArray(PropTag tag, const std::vector<std::string>& items);
    // PtypObject: four bytes of subnode NID and four of size.  Used for an
    // embedded message, whose own PC lives in that subnode.
    void setObject(PropTag tag, Nid nid, std::uint32_t size);
    // Records a value the caller has already written to a subnode.  Large
    // payloads -- attachment bodies above all -- would otherwise be copied into
    // the property context and held there until serialize(), doubling peak
    // memory for the duration of the message.
    void setSpilledValue(PropTag tag, Nid nid);

    bool has(PropTag tag) const;
    std::size_t size() const { return values_.size(); }

    // Produces the HN byte stream for this PC.
    std::vector<std::uint8_t> serialize(SubnodeAllocator& subs) const;

 private:
    struct Value {
        // kInline: the value fits in the record's 4-byte HNID field.
        // kBytes:  serialize() places it in the heap or spills it.
        // kSpilled: already in a subnode; `hnid` is its NID.
        enum class Kind { kInline, kBytes, kSpilled };
        PropTag tag = 0;
        Kind kind = Kind::kBytes;
        std::uint32_t hnid = 0;
        std::vector<std::uint8_t> bytes;
    };
    void put(PropTag tag, Value v);

    std::vector<Value> values_;
};

// --------------------------------------------------------------- TableContext

class TableContext {
 public:
    // PidTagLtpRowId and PidTagLtpRowVer are added automatically and always
    // occupy row offsets 0 and 4.
    TableContext();

    void addColumn(PropTag tag);

    // Returns the index of the new row.  `row_id` is what the row-index BTH is
    // keyed on; for a folder's contents table it is the message NID.
    std::size_t addRow(std::uint32_t row_id);
    std::size_t rowCount() const { return rows_.size(); }

    void setInt16(std::size_t row, PropTag tag, std::uint16_t v);
    void setInt32(std::size_t row, PropTag tag, std::uint32_t v);
    void setBool(std::size_t row, PropTag tag, bool v);
    void setInt64(std::size_t row, PropTag tag, std::uint64_t v);
    void setTime(std::size_t row, PropTag tag, std::uint64_t filetime);
    void setString(std::size_t row, PropTag tag, const std::string& utf8);
    void setBinary(std::size_t row, PropTag tag, const void* data, std::size_t len);

    std::vector<std::uint8_t> serialize(SubnodeAllocator& subs) const;

 private:
    struct Cell {
        bool present = false;
        bool variable = false;
        std::vector<std::uint8_t> bytes;
    };
    struct Row {
        std::uint32_t row_id = 0;
        std::vector<Cell> cells;
    };
    struct Column {
        PropTag tag = 0;
        std::uint16_t ib = 0;
        std::uint8_t cb = 0;
        std::uint8_t ibit = 0;
    };

    std::size_t columnIndex(PropTag tag) const;
    Cell& cell(std::size_t row, PropTag tag);
    std::vector<Column> layout(std::uint16_t* row_size, std::uint16_t rgib[4]) const;

    std::vector<PropTag> columns_;
    std::vector<Row> rows_;
};

}  // namespace imap2pst::pst
