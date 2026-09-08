#pragma once

// A minimal PST reader for tests, written straight from [MS-PST] and sharing no
// code with the writer, so a mistake in the writer cannot hide behind a
// matching mistake in a helper it also uses.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
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

    std::vector<BlockRef> blocks() const {
        std::vector<std::vector<std::uint8_t>> leaves;
        walk(bbtRoot(), kPTypeBBT, 0, &leaves, nullptr);
        std::vector<BlockRef> out;
        for (const auto& e : leaves) {
            out.push_back({peek64(e.data()), peek64(e.data() + 8), peek16(e.data() + 16)});
        }
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


}  // namespace imap2pst::pst::test
