#pragma once

// A minimal PST reader for tests, written straight from [MS-PST] and sharing no
// code with the writer, so a mistake in the writer cannot hide behind a
// matching mistake in a helper it also uses.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

#include "pst/crc.h"
#include "pst/pst_format.h"

namespace imap2pst::pst::test {

// Writes a PST to a scratch path that is removed when the fixture dies.
class TempPst {
 public:
    TempPst() {
        char pattern[] = "/tmp/imap2pst_test_XXXXXX";
        const int fd = ::mkstemp(pattern);
        if (fd >= 0) ::close(fd);
        path_ = pattern;
    }
    ~TempPst() { std::remove(path_.c_str()); }
    const std::string& path() const { return path_; }

 private:
    std::string path_;
};

// A directory that goes away with the test, for anything the code under test
// writes beside the PST itself.
class TempDir {
 public:
    TempDir() {
        char pattern[] = "/tmp/imap2pst_test_dir_XXXXXX";
        const char* made = ::mkdtemp(pattern);
        path_ = made ? made : pattern;
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::string& path() const { return path_; }

 private:
    std::string path_;
};

std::vector<std::uint8_t> readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

// --------------------------------------------------------------- tiny reader

struct Bref {
    Bid bid = 0;
    std::uint64_t ib = 0;
};

struct BlockRef {
    Bid bid = 0;
    std::uint64_t ib = 0;
    std::uint16_t cb = 0;
};

class Reader {
 public:
    explicit Reader(std::vector<std::uint8_t> bytes) : b_(std::move(bytes)) {}

    const std::vector<std::uint8_t>& bytes() const { return b_; }
    std::size_t size() const { return b_.size(); }
    const std::uint8_t* at(std::uint64_t off) const { return b_.data() + off; }

    Bref nbtRoot() const { return {peek64(at(180 + 36)), peek64(at(180 + 44))}; }
    Bref bbtRoot() const { return {peek64(at(180 + 52)), peek64(at(180 + 60))}; }
    std::uint64_t ibFileEof() const { return peek64(at(180 + 4)); }
    std::uint64_t ibAMapLast() const { return peek64(at(180 + 12)); }
    std::uint8_t fAMapValid() const { return *at(180 + 68); }

    // Walks a B-tree page, checking its trailer, and collects leaf entries.
    void walk(const Bref& ref, std::uint8_t expect_ptype, int depth,
              std::vector<std::vector<std::uint8_t>>* leaves,
              std::vector<std::uint64_t>* keys) const {
        ASSERT_LT(depth, 16) << "B-tree deeper than 16 levels";
        ASSERT_LE(ref.ib + kPageSize, b_.size()) << "page past end of file";
        const std::uint8_t* p = at(ref.ib);

        const std::uint8_t* t = p + 496;
        EXPECT_EQ(t[0], expect_ptype);
        EXPECT_EQ(t[1], expect_ptype) << "ptypeRepeat must mirror ptype";
        EXPECT_EQ(peek16(t + 2), computeSig(ref.ib, ref.bid)) << "bad page signature";
        EXPECT_EQ(peek32(t + 4), computeCrc(p, 496)) << "bad page CRC";
        EXPECT_EQ(peek64(t + 8), ref.bid) << "page trailer BID mismatch";

        const std::uint8_t count = p[488];
        const std::uint8_t max = p[489];
        const std::uint8_t entry_size = p[490];
        const std::uint8_t level = p[491];
        EXPECT_LE(count, max);
        EXPECT_EQ(max, kBTPageDataSize / entry_size);

        std::uint64_t previous = 0;
        for (std::uint8_t i = 0; i < count; ++i) {
            const std::uint8_t* e = p + i * entry_size;
            const std::uint64_t key = peek64(e);
            if (i > 0) EXPECT_GT(key, previous) << "B-tree keys out of order";
            previous = key;

            if (level > 0) {
                EXPECT_EQ(entry_size, kSizeBTEntry);
                walk({peek64(e + 8), peek64(e + 16)}, expect_ptype, depth + 1, leaves, keys);
            } else {
                if (keys) keys->push_back(key);
                if (leaves) leaves->emplace_back(e, e + entry_size);
            }
        }
    }

    void collectPages(const Bref& ref, std::vector<std::uint64_t>* out) const {
        out->push_back(ref.ib);
        const std::uint8_t* p = at(ref.ib);
        const std::uint8_t count = p[488];
        const std::uint8_t entry_size = p[490];
        if (p[491] == 0) return;
        for (std::uint8_t i = 0; i < count; ++i) {
            const std::uint8_t* e = p + i * entry_size;
            collectPages({peek64(e + 8), peek64(e + 16)}, out);
        }
    }

    std::vector<BlockRef> blocks() const {
        std::vector<std::vector<std::uint8_t>> leaves;
        walk(bbtRoot(), kPTypeBBT, 0, &leaves, nullptr);
        std::vector<BlockRef> out;
        for (const auto& e : leaves) {
            out.push_back({peek64(e.data()), peek64(e.data() + 8), peek16(e.data() + 16)});
        }
        return out;
    }

    // Every page of both B-trees, by file offset.
    std::vector<std::uint64_t> btreePages() const {
        std::vector<std::uint64_t> out;
        collectPages(nbtRoot(), &out);
        collectPages(bbtRoot(), &out);
        return out;
    }

    struct NodeEntry {
        Nid nid = 0;
        Bid data = 0;
        Bid sub = 0;
        Nid parent = 0;
    };

    std::vector<NodeEntry> nodeEntries() const {
        std::vector<std::vector<std::uint8_t>> leaves;
        walk(nbtRoot(), kPTypeNBT, 0, &leaves, nullptr);
        std::vector<NodeEntry> out;
        for (const auto& e : leaves) {
            out.push_back({static_cast<Nid>(peek64(e.data())), peek64(e.data() + 8),
                           peek64(e.data() + 16),
                           static_cast<Nid>(peek32(e.data() + 24))});
        }
        return out;
    }

    // ---- LTP: enough of it to read a property back ------------------------
    //
    // Written from [MS-PST] alongside the NDB reader above, and sharing nothing
    // with the writer, so a test that reads a property back is checking the
    // file rather than agreeing with the code that produced it.

    // Concatenated contents of a data tree, following XBLOCKs.
    std::vector<std::uint8_t> blockData(Bid bid) const {
        std::vector<std::uint8_t> out;
        const auto all = blocks();
        auto find = [&](Bid want) -> const BlockRef* {
            for (const auto& b : all) {
                if (b.bid == want) return &b;
            }
            return nullptr;
        };
        std::function<void(Bid)> append = [&](Bid want) {
            const BlockRef* b = find(want);
            if (!b) return;
            const std::uint8_t* p = at(b->ib);
            if ((want & 2) && b->cb >= 8 && p[0] == 1) {   // XBLOCK / XXBLOCK
                const std::uint16_t count = peek16(p + 2);
                for (std::uint16_t i = 0; i < count; ++i) append(peek64(p + 8 + 8 * i));
                return;
            }
            out.insert(out.end(), p, p + b->cb);
        };
        append(bid);
        return out;
    }

    // Subnode NID -> (data BID, subnode BID).
    std::map<Nid, std::pair<Bid, Bid>> subnodes(Bid bid) const {
        std::map<Nid, std::pair<Bid, Bid>> out;
        if (bid == 0) return out;
        const auto all = blocks();
        std::function<void(Bid)> walkSub = [&](Bid want) {
            for (const auto& b : all) {
                if (b.bid != want) continue;
                const std::uint8_t* p = at(b.ib);
                if (b.cb < 8 || p[0] != 2) return;
                const std::uint8_t level = p[1];
                const std::uint16_t count = peek16(p + 2);
                for (std::uint16_t i = 0; i < count; ++i) {
                    if (level == 0) {
                        out[static_cast<Nid>(peek64(p + 8 + 24 * i))] = {
                            peek64(p + 16 + 24 * i), peek64(p + 24 + 24 * i)};
                    } else {
                        walkSub(peek64(p + 16 + 16 * i));
                    }
                }
                return;
            }
        };
        walkSub(bid);
        return out;
    }

    std::vector<Nid> nodes() const {
        std::vector<std::vector<std::uint8_t>> leaves;
        walk(nbtRoot(), kPTypeNBT, 0, &leaves, nullptr);
        std::vector<Nid> out;
        for (const auto& e : leaves) out.push_back(static_cast<Nid>(peek64(e.data())));
        return out;
    }

 private:
    std::vector<std::uint8_t> b_;
};


// One allocation out of a heap-on-node buffer.
inline std::vector<std::uint8_t> heapItem(const std::vector<std::uint8_t>& buf,
                                          std::uint32_t hid) {
    const std::uint32_t index = (hid >> 5) & 0x7FF;
    const std::size_t block = hid >> 16;
    if (index == 0) return {};
    const std::size_t base = block * kMaxBlockData;
    if (base + 2 > buf.size()) return {};
    const std::size_t map = base + peek16(buf.data() + base);
    if (map + 4 > buf.size()) return {};
    if (index > peek16(buf.data() + map)) return {};
    const std::size_t start = base + peek16(buf.data() + map + 4 + 2 * (index - 1));
    const std::size_t end = base + peek16(buf.data() + map + 6 + 2 * (index - 1));
    if (end > buf.size() || start > end) return {};
    return std::vector<std::uint8_t>(buf.begin() + start, buf.begin() + end);
}

// The value of one property in a PC, as (type, bytes).  Values held in
// subnodes come back empty: the caller knows the subnode tree and can follow it.
inline std::pair<std::uint16_t, std::vector<std::uint8_t>> pcProperty(
        const std::vector<std::uint8_t>& heap, std::uint16_t prop_id) {
    if (heap.size() < 8 || heap[3] != kHnSigPC) return {0, {}};
    const auto header = heapItem(heap, peek32(heap.data() + 4));
    if (header.size() < 8 || header[0] != kHnSigBTH) return {0, {}};
    const std::uint8_t key_size = header[1], data_size = header[2], levels = header[3];
    std::vector<std::uint8_t> records = heapItem(heap, peek32(header.data() + 4));
    for (std::uint8_t level = 0; level < levels; ++level) {
        // Intermediate levels: key plus the HID of the next level down.
        std::vector<std::uint8_t> next;
        for (std::size_t o = 0; o + key_size + 4 <= records.size(); o += key_size + 4) {
            const std::uint16_t key = peek16(records.data() + o);
            if (key > prop_id) break;
            next = heapItem(heap, peek32(records.data() + o + key_size));
        }
        records = next;
    }
    const std::size_t stride = key_size + data_size;
    for (std::size_t o = 0; o + stride <= records.size(); o += stride) {
        if (peek16(records.data() + o) != prop_id) continue;
        const std::uint16_t type = peek16(records.data() + o + key_size);
        const std::uint32_t hnid = peek32(records.data() + o + key_size + 2);
        if (type == kPtLong || type == kPtBoolean || type == kPtShort) {
            std::vector<std::uint8_t> inline_value(4);
            for (int i = 0; i < 4; ++i) {
                inline_value[i] = static_cast<std::uint8_t>(hnid >> (8 * i));
            }
            return {type, inline_value};
        }
        if (hnid != 0 && (hnid & 0x1F) == 0) return {type, heapItem(heap, hnid)};
        return {type, {}};   // absent, or spilled into a subnode
    }
    return {0, {}};
}

inline std::string utf16ToUtf8(const std::vector<std::uint8_t>& v) {
    std::string out;
    for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
        const std::uint32_t c = peek16(v.data() + i);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

}  // namespace imap2pst::pst::test
