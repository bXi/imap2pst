// Binary-level checks on a PST this writer produced: header fields, page and
// block trailers, CRCs, B-tree shape and the allocation map.
//
// These are deliberately independent of libpff -- they re-read the file with a
// small parser written straight from [MS-PST] so a mistake in the writer cannot
// hide behind a matching mistake in a shared helper.

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "pst/crc.h"
#include "pst/pst_format.h"
#include "pst/pst_writer.h"
#include "pst_reader.h"

namespace imap2pst::pst {
namespace {

using test::readAll;
using test::Reader;
using test::TempPst;

Message makeMessage(int i, bool with_attachment) {
    Message m;
    m.subject = "Subject " + std::to_string(i);
    m.from = {"Sender " + std::to_string(i), "s@example.com"};
    m.to = {{"Recipient", "r@example.com"}};
    m.body_text = "Body " + std::to_string(i);
    m.delivery_time = 1700000000 + i;
    m.date = m.delivery_time;
    m.headers = {{"Subject", m.subject}, {"X-Seq", std::to_string(i)}};
    if (with_attachment) {
        Attachment a;
        a.filename = "file.bin";
        a.content_type = "application/octet-stream";
        a.data.assign(1000, static_cast<std::uint8_t>(i));
        m.attachments.push_back(std::move(a));
    }
    return m;
}

void writeSample(const std::string& path, int message_count) {
    PstWriter w(path);
    const auto inbox = w.createFolder(w.ipmSubtree(), "Inbox");
    const auto sub = w.createFolder(inbox, "Nested");
    for (int i = 0; i < message_count; ++i) {
        w.addMessage(i % 3 == 0 ? sub : inbox, makeMessage(i, i % 5 == 0));
    }
    w.finish();
}

// ------------------------------------------------------------------- tests

TEST(PstHeader, MagicVersionAndChecksums) {
    TempPst tmp;
    writeSample(tmp.path(), 12);
    const Reader r(readAll(tmp.path()));
    ASSERT_GT(r.size(), kHeaderSize);

    const std::uint8_t* h = r.at(0);
    EXPECT_EQ(peek32(h), 0x4E444221u) << "dwMagic must be \"!BDN\"";
    EXPECT_EQ(h[8], 'S');
    EXPECT_EQ(h[9], 'M');
    EXPECT_EQ(peek16(h + 10), 23) << "wVer 23 marks a Unicode PST";
    EXPECT_EQ(h[512], 0x80) << "bSentinel";
    EXPECT_EQ(h[513], 0x00) << "bCryptMethod must be NDB_CRYPT_NONE";

    EXPECT_EQ(peek32(h + 4), computeCrc(h + 8, 471)) << "dwCRCPartial";
    EXPECT_EQ(peek32(h + 524), computeCrc(h + 8, 516)) << "dwCRCFull";

    // rgbFM saturates: a freshly written file has one long free run per map.
    for (int i = 0; i < 128; ++i) {
        EXPECT_EQ(h[256 + i], 0xFF) << "rgbFM[" << i << "]";
    }
    // rgbFP stays saturated; see the note in writeHeader.  scanpst reports the
    // inconsistency, Outlook needs it this way.
    for (int i = 0; i < 128; ++i) {
        EXPECT_EQ(h[384 + i], 0xFF) << "rgbFP[" << i << "]";
    }
}

TEST(PstHeader, RootPointsAtTheRealEndOfFile) {
    TempPst tmp;
    writeSample(tmp.path(), 12);
    const Reader r(readAll(tmp.path()));
    EXPECT_EQ(r.ibFileEof(), r.size());
    EXPECT_EQ(r.fAMapValid(), 0x02);
    EXPECT_GE(r.ibAMapLast(), kFirstAMapPos);
    EXPECT_EQ((r.ibAMapLast() - kFirstAMapPos) % kAMapSpan, 0u);
}

TEST(PstBTrees, PagesAreStructurallySound) {
    TempPst tmp;
    // Enough messages to push both B-trees past a single leaf page.
    writeSample(tmp.path(), 400);
    const Reader r(readAll(tmp.path()));

    std::vector<std::uint64_t> nbt_keys, bbt_keys;
    r.walk(r.nbtRoot(), kPTypeNBT, 0, nullptr, &nbt_keys);
    r.walk(r.bbtRoot(), kPTypeBBT, 0, nullptr, &bbt_keys);

    EXPECT_GT(nbt_keys.size(), 400u);
    EXPECT_GT(bbt_keys.size(), 400u);
    // A single NBT leaf page holds 15 entries, so this many nodes proves the
    // multi-level path is exercised.
    EXPECT_GT(nbt_keys.size(), static_cast<std::size_t>(kBTPageDataSize / kSizeNBTEntry));

    std::set<std::uint64_t> unique_nbt(nbt_keys.begin(), nbt_keys.end());
    EXPECT_EQ(unique_nbt.size(), nbt_keys.size()) << "duplicate NIDs in the NBT";
    std::set<std::uint64_t> unique_bbt(bbt_keys.begin(), bbt_keys.end());
    EXPECT_EQ(unique_bbt.size(), bbt_keys.size()) << "duplicate BIDs in the BBT";
}

TEST(PstBlocks, EveryBlockHasAValidTrailer) {
    TempPst tmp;
    writeSample(tmp.path(), 40);
    const Reader r(readAll(tmp.path()));

    const auto blocks = r.blocks();
    ASSERT_FALSE(blocks.empty());
    for (const auto& blk : blocks) {
        const std::uint64_t total = alignUp(blk.cb + 16u, kBlockAlign);
        ASSERT_LE(blk.ib + total, r.size()) << "block runs past end of file";
        EXPECT_EQ(blk.ib % kBlockAlign, 0u) << "block not 64-byte aligned";
        EXPECT_LE(blk.cb, kMaxBlockData);

        const std::uint8_t* t = r.at(blk.ib + total - 16);
        EXPECT_EQ(peek16(t), blk.cb) << "trailer cb disagrees with the BBT";
        EXPECT_EQ(peek16(t + 2), computeSig(blk.ib, blk.bid)) << "bad block signature";
        EXPECT_EQ(peek32(t + 4), computeCrc(r.at(blk.ib), blk.cb)) << "bad block CRC";
        EXPECT_EQ(peek64(t + 8), blk.bid) << "trailer BID mismatch";
    }
}

TEST(PstBlocks, InternalFlagMatchesBlockContent) {
    TempPst tmp;
    writeSample(tmp.path(), 40);
    const Reader r(readAll(tmp.path()));
    for (const auto& blk : r.blocks()) {
        if (!bidIsInternal(blk.bid)) continue;
        // XBLOCK/XXBLOCK use btype 0x01, SLBLOCK/SIBLOCK use 0x02.
        const std::uint8_t btype = *r.at(blk.ib);
        EXPECT_TRUE(btype == 0x01 || btype == 0x02)
            << "internal block with btype 0x" << std::hex << int(btype);
        EXPECT_LE(*r.at(blk.ib + 1), 2) << "block level out of range";
    }
}

TEST(PstAllocation, AMapMarksEveryBlockAsUsed) {
    TempPst tmp;
    writeSample(tmp.path(), 40);
    const Reader r(readAll(tmp.path()));

    auto is_allocated = [&](std::uint64_t ib) {
        const std::uint64_t map = (ib - kFirstAMapPos) / kAMapSpan;
        const std::uint64_t base = kFirstAMapPos + map * kAMapSpan;
        const std::uint64_t bit = (ib - base) / kBlockAlign;
        const std::uint8_t byte = *r.at(base + bit / 8);
        return (byte & (0x80u >> (bit % 8))) != 0;
    };

    for (const auto& blk : r.blocks()) {
        const std::uint64_t total = alignUp(blk.cb + 16u, kBlockAlign);
        for (std::uint64_t off = 0; off < total; off += kBlockAlign) {
            EXPECT_TRUE(is_allocated(blk.ib + off))
                << "block at 0x" << std::hex << blk.ib << " is not marked in the AMap";
        }
    }
    // The AMap page itself and the PMap page must be marked too.
    EXPECT_TRUE(is_allocated(kFirstAMapPos));
    EXPECT_TRUE(is_allocated(kFirstPMapPos));
}

TEST(PstAllocation, NothingOverlapsAReservedPage) {
    TempPst tmp;
    // Large enough to run past the first AMap span and hit the second one.
    PstWriter w(tmp.path());
    const auto folder = w.createFolder(w.ipmSubtree(), "Big");
    for (int i = 0; i < 40; ++i) {
        Message m = makeMessage(i, false);
        m.body_text = std::string(64 * 1024, 'a');
        w.addMessage(folder, m);
    }
    w.finish();

    const Reader r(readAll(tmp.path()));
    ASSERT_GT(r.size(), kFirstAMapPos + kAMapSpan) << "test did not reach a second AMap";

    for (const auto& blk : r.blocks()) {
        const std::uint64_t total = alignUp(blk.cb + 16u, kBlockAlign);
        for (std::uint64_t p = kFirstAMapPos; p < r.size(); p += kAMapSpan) {
            const bool overlaps = blk.ib < p + kPageSize && p < blk.ib + total;
            EXPECT_FALSE(overlaps) << "block overlaps the AMap page at 0x" << std::hex << p;
        }
    }
}

TEST(PstStreamingTable, LargeFolderStaysStructurallySound) {
    TempPst tmp;
    // Well past one heap block and one row-matrix block, so the contents table
    // is written through the streaming path rather than buffered whole.
    writeSample(tmp.path(), 5000);
    const Reader r(readAll(tmp.path()));

    EXPECT_EQ(r.ibFileEof(), r.size());
    for (const auto& blk : r.blocks()) {
        const std::uint64_t total = alignUp(blk.cb + 16u, kBlockAlign);
        const std::uint8_t* t = r.at(blk.ib + total - 16);
        ASSERT_EQ(peek32(t + 4), computeCrc(r.at(blk.ib), blk.cb))
            << "bad block CRC at 0x" << std::hex << blk.ib;
    }

    std::size_t messages = 0;
    for (Nid nid : r.nodes()) {
        if (nidType(nid) == kNidTypeNormalMessage) ++messages;
    }
    EXPECT_EQ(messages, 5000u);
}

TEST(PstAllocation, MapPagesCarryTheirOwnOffsetAsBid) {
    // [MS-PST] 2.2.2.7.2/2.2.2.7.3.  Outlook validates this and rejects the
    // whole allocation map when it disagrees, which cascades into it rebuilding
    // both B-trees and declaring present nodes missing.
    TempPst tmp;
    PstWriter w(tmp.path());
    const auto folder = w.createFolder(w.ipmSubtree(), "Big");
    for (int i = 0; i < 40; ++i) {
        Message m = makeMessage(i, false);
        m.body_text = std::string(64 * 1024, 'a');
        w.addMessage(folder, m);
    }
    w.finish();

    const Reader r(readAll(tmp.path()));
    ASSERT_GT(r.size(), kFirstAMapPos + kAMapSpan) << "test needs a second AMap";

    std::size_t checked = 0;
    for (std::uint64_t ib = kFirstAMapPos; ib < r.size(); ib += kAMapSpan) {
        const std::uint8_t* t = r.at(ib + 496);
        EXPECT_EQ(t[0], kPTypeAMap);
        EXPECT_EQ(peek64(t + 8), ib) << "AMap page at 0x" << std::hex << ib;
        EXPECT_EQ(peek16(t + 2), 0) << "AMap pages carry no signature";
        ++checked;
    }
    for (std::uint64_t ib = kFirstPMapPos; ib < r.size(); ib += kPMapSpan) {
        const std::uint8_t* t = r.at(ib + 496);
        EXPECT_EQ(t[0], kPTypePMap);
        EXPECT_EQ(peek64(t + 8), ib) << "PMap page at 0x" << std::hex << ib;
        EXPECT_EQ(peek16(t + 2), 0) << "PMap pages carry no signature";
        ++checked;
    }
    EXPECT_GE(checked, 3u);
}

TEST(PstAllocation, BTreePagesAreMarkedInTheAMap) {
    // The block-level check does not cover these: pages are not in the BBT, so
    // nothing else in the suite would notice if they went unmarked.
    TempPst tmp;
    writeSample(tmp.path(), 400);
    const Reader r(readAll(tmp.path()));

    auto is_allocated = [&](std::uint64_t ib) {
        const std::uint64_t map = (ib - kFirstAMapPos) / kAMapSpan;
        const std::uint64_t base = kFirstAMapPos + map * kAMapSpan;
        const std::uint64_t bit = (ib - base) / kBlockAlign;
        return (*r.at(base + bit / 8) & (0x80u >> (bit % 8))) != 0;
    };

    for (const auto& page : r.btreePages()) {
        EXPECT_TRUE(is_allocated(page)) << "B-tree page at 0x" << std::hex << page
                                        << " is not marked in the AMap";
    }
}

TEST(PstNodes, UserObjectsUseTheNonReservedNidRange) {
    TempPst tmp;
    writeSample(tmp.path(), 20);
    const Reader r(readAll(tmp.path()));
    for (Nid nid : r.nodes()) {
        const NidType type = nidType(nid);
        if (type != kNidTypeNormalMessage && type != kNidTypeNormalFolder) continue;
        if (nid == kNidRootFolder) continue;  // reserved by the format
        EXPECT_GE(nidIndex(nid), kFirstUserNidIndex)
            << "NID 0x" << std::hex << nid << " sits in the reserved index range";
    }
}

TEST(PstHeader, NidHighWaterMarksClearTheReservedRange) {
    TempPst tmp;
    writeSample(tmp.path(), 20);
    const Reader r(readAll(tmp.path()));
    const std::uint8_t* h = r.at(0);
    for (int i = 0; i < 32; ++i) {
        EXPECT_GE(peek32(h + 44 + i * 4), kFirstUserNidIndex)
            << "rgnid[" << i << "] is below the reserved index range";
    }
}

TEST(PstHeap, NoZeroLengthAllocationsAndCFreeCountsThem) {
    // cFree is the number of freed allocation slots, not the number of free
    // bytes.  Outlook counts zero-length rgibAlloc entries and rejects the
    // whole heap when the stored value disagrees, which silently makes every
    // property in that node unreadable.
    TempPst tmp;
    writeSample(tmp.path(), 30);
    const Reader r(readAll(tmp.path()));

    std::size_t heaps = 0;
    for (const auto& blk : r.blocks()) {
        if (blk.cb < 12) continue;
        const std::uint8_t* p = r.at(blk.ib);
        if (p[2] != kHnSignature) continue;  // not an HN first block
        const std::uint16_t ibHnpm = peek16(p);
        if (ibHnpm + 4u > blk.cb) continue;
        const std::uint16_t alloc_count = peek16(p + ibHnpm);
        const std::uint16_t free_count = peek16(p + ibHnpm + 2);
        std::uint16_t zero_length = 0;
        for (std::uint16_t i = 0; i < alloc_count; ++i) {
            const std::uint16_t a = peek16(p + ibHnpm + 4 + 2 * i);
            const std::uint16_t b = peek16(p + ibHnpm + 6 + 2 * i);
            if (a == b) ++zero_length;
        }
        EXPECT_EQ(zero_length, 0) << "zero-length heap allocation at 0x" << std::hex << blk.ib;
        EXPECT_EQ(free_count, zero_length) << "cFree disagrees at 0x" << std::hex << blk.ib;
        ++heaps;
    }
    EXPECT_GT(heaps, 10u) << "expected to have inspected a good number of heaps";
}

TEST(PstHeader, AMapFreeSpaceIsPublished) {
    TempPst tmp;
    writeSample(tmp.path(), 30);
    const Reader r(readAll(tmp.path()));

    // Count the free 64-byte units the allocation maps actually describe.
    std::uint64_t expected = 0;
    for (std::uint64_t base = kFirstAMapPos; base < r.size(); base += kAMapSpan) {
        for (std::size_t i = 0; i < 496; ++i) {
            const std::uint8_t byte = *r.at(base + i);
            for (int b = 0; b < 8; ++b) {
                if ((byte & (0x80u >> b)) == 0) expected += kBlockAlign;
            }
        }
    }
    EXPECT_EQ(peek64(r.at(180 + 20)), expected) << "ROOT.cbAMapFree";
}

TEST(PstHeap, BthKeysNeverStartAtZero) {
    // Outlook rejects a BTH whose first key is zero ("keys overlap, dwkey=0,
    // dwkeyMin=0") and discards the table that owns it, which is how a
    // recipient table full of correct columns got reported as missing all of
    // them.  Row ids therefore start at one.
    TempPst tmp;
    writeSample(tmp.path(), 10);
    const Reader r(readAll(tmp.path()));

    std::size_t bths = 0;
    for (const auto& blk : r.blocks()) {
        if (blk.cb < 12) continue;
        const std::uint8_t* p = r.at(blk.ib);
        if (p[2] != kHnSignature) continue;
        const Hid user_root = peek32(p + 4);
        if (user_root == 0 || hidBlockIndex(user_root) != 0) continue;
        const std::uint16_t ibHnpm = peek16(p);
        if (ibHnpm + 4u > blk.cb) continue;
        const std::uint16_t alloc_count = peek16(p + ibHnpm);
        const std::uint16_t index = hidAllocIndex(user_root);
        if (index == 0 || index > alloc_count) continue;
        const std::uint16_t start = peek16(p + ibHnpm + 4 + 2 * (index - 1));
        if (start + 8u > blk.cb) continue;
        const std::uint8_t* bth = p + start;
        if (bth[0] != kHnSigBTH) continue;  // a TC's user root, not a BTH

        const std::uint8_t cb_key = bth[1];
        const std::uint8_t levels = bth[3];
        const Hid root = peek32(bth + 4);
        if (root == 0 || levels != 0 || hidBlockIndex(root) != 0) continue;
        const std::uint16_t ri = hidAllocIndex(root);
        if (ri == 0 || ri > alloc_count) continue;
        const std::uint16_t rstart = peek16(p + ibHnpm + 4 + 2 * (ri - 1));
        const std::uint16_t rend = peek16(p + ibHnpm + 6 + 2 * (ri - 1));
        if (rend <= rstart) continue;

        std::uint64_t first_key = 0;
        for (int i = cb_key; i-- > 0;) first_key = (first_key << 8) | p[rstart + i];
        EXPECT_NE(first_key, 0u)
            << "BTH at 0x" << std::hex << blk.ib << " starts at key zero";
        ++bths;
    }
    EXPECT_GT(bths, 5u) << "expected to have inspected several BTHs";
}

TEST(PstNodes, RootFolderIsItsOwnParent) {
    // Outlook walks up the folder tree until a folder's parent is itself.  With
    // a parent of zero it follows a node identifier that was never written.
    TempPst tmp;
    writeSample(tmp.path(), 5);
    const Reader r(readAll(tmp.path()));

    bool seen = false;
    for (const auto& e : r.nodeEntries()) {
        if (e.nid != kNidRootFolder) continue;
        seen = true;
        EXPECT_EQ(e.parent, kNidRootFolder) << "root folder must be its own parent";
    }
    EXPECT_TRUE(seen) << "root folder node missing";
}

TEST(PstAllocation, FreeSpaceIsBackedByTheFile) {
    // The allocation maps describe every slot in their span, so a file that
    // stops mid-span advertises free space that does not exist.  A reader that
    // opens the store for writing takes one of those slots and seeks past the
    // end of the file.
    TempPst tmp;
    writeSample(tmp.path(), 5);
    const Reader r(readAll(tmp.path()));

    EXPECT_EQ(r.ibFileEof(), r.size());
    EXPECT_EQ((r.size() - kFirstAMapPos) % kAMapSpan, 0u)
        << "file must end on an allocation-map span boundary";

    // Every slot the maps call free has to lie inside the file.
    for (std::uint64_t base = kFirstAMapPos; base < r.size(); base += kAMapSpan) {
        for (std::size_t bit = 0; bit < 496 * 8; ++bit) {
            const std::uint8_t byte = *r.at(base + bit / 8);
            if (byte & (0x80u >> (bit % 8))) continue;  // allocated
            const std::uint64_t slot = base + bit * kBlockAlign;
            ASSERT_LT(slot, r.size())
                << "AMap marks 0x" << std::hex << slot << " free, past end of file";
        }
    }
}

TEST(PstNodes, ReservedNodesArePresent) {
    TempPst tmp;
    writeSample(tmp.path(), 5);
    const Reader r(readAll(tmp.path()));
    const auto nids = r.nodes();
    const std::set<Nid> present(nids.begin(), nids.end());

    EXPECT_TRUE(present.count(kNidMessageStore)) << "message store node missing";
    EXPECT_TRUE(present.count(kNidNameToIdMap)) << "name-to-id map node missing";
    EXPECT_TRUE(present.count(kNidRootFolder)) << "root folder node missing";
    // Every folder brings a hierarchy, contents and associated-contents table.
    EXPECT_TRUE(present.count(makeNid(kNidTypeHierarchyTable, nidIndex(kNidRootFolder))));
    EXPECT_TRUE(present.count(makeNid(kNidTypeContentsTable, nidIndex(kNidRootFolder))));
    EXPECT_TRUE(present.count(makeNid(kNidTypeAssocContentsTable, nidIndex(kNidRootFolder))));
}

TEST(PstFormat, Utf16AndFiletimeConversions) {
    const auto ascii = utf8ToUtf16le("AB");
    ASSERT_EQ(ascii.size(), 4u);
    EXPECT_EQ(ascii[0], 'A');
    EXPECT_EQ(ascii[1], 0);

    // U+1F600, outside the BMP, must become a surrogate pair.
    const auto emoji = utf8ToUtf16le("\xF0\x9F\x98\x80");
    ASSERT_EQ(emoji.size(), 4u);
    EXPECT_EQ(peek16(emoji.data()), 0xD83D);
    EXPECT_EQ(peek16(emoji.data() + 2), 0xDE00);

    // The Unix epoch in FILETIME ticks.
    EXPECT_EQ(unixToFiletime(0), 116444736000000000ULL);
    EXPECT_EQ(unixToFiletime(1), 116444736010000000ULL);
}

TEST(PstFormat, NidAndHidPacking) {
    const Nid nid = makeNid(kNidTypeNormalMessage, 42);
    EXPECT_EQ(nidType(nid), kNidTypeNormalMessage);
    EXPECT_EQ(nidIndex(nid), 42u);
    EXPECT_EQ(kNidRootFolder, makeNid(kNidTypeNormalFolder, 9));

    const Hid hid = makeHid(3, 7);
    EXPECT_EQ(hidBlockIndex(hid), 3);
    EXPECT_EQ(hidAllocIndex(hid), 7);
    EXPECT_TRUE(hnidIsHid(hid));
    EXPECT_FALSE(hnidIsHid(kNidRecipientTable));
}

}  // namespace
}  // namespace imap2pst::pst
