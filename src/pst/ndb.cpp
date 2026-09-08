#include "pst/ndb.h"

#include <algorithm>
#include <array>
#include <cassert>

#include "pst/crc.h"

namespace imap2pst::pst {
namespace {

// Entries that fit in one internal/leaf B-tree page.
std::size_t entriesPerPage(std::uint8_t entry_size) {
    return kBTPageDataSize / entry_size;
}

// XBLOCK / XXBLOCK header is 8 bytes, the rest is an array of BIDs.
constexpr std::size_t kXBlockFanout = (kMaxBlockData - 8) / 8;   // 1021
// SLBLOCK header is 8 bytes, SLENTRY is 24.
constexpr std::size_t kSlBlockFanout = (kMaxBlockData - 8) / 24;  // 340
// SIBLOCK header is 8 bytes, SIENTRY is 16.
constexpr std::size_t kSiBlockFanout = (kMaxBlockData - 8) / 16;  // 511

}  // namespace

NdbWriter::NdbWriter(const std::string& path) : path_(path) {
    out_.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!out_) throw PstError("cannot open PST for writing: " + path);
    // The region below kFirstDataPos holds the header and the first fixed
    // pages.  Reserve it up front so the file is never sparse there.
    std::vector<std::uint8_t> zeros(kFirstDataPos, 0);
    out_.write(reinterpret_cast<const char*>(zeros.data()),
               static_cast<std::streamsize>(zeros.size()));
    if (!out_) throw PstError("cannot preallocate PST header region");
    ensureAMapCount(1);
}

NdbWriter::~NdbWriter() {
    if (out_.is_open()) out_.close();
}

// --------------------------------------------------------------- space mgmt

bool NdbWriter::isReservedPage(std::uint64_t ib) {
    if (ib == kDListPos) return true;
    if (ib < kFirstAMapPos) return false;
    if ((ib - kFirstAMapPos) % kAMapSpan == 0) return true;
    if (ib >= kFirstPMapPos && (ib - kFirstPMapPos) % kPMapSpan == 0) return true;
    return false;
}

std::uint64_t NdbWriter::allocate(std::uint64_t cb) {
    cb = alignUp(cb, kBlockAlign);
    for (;;) {
        bool clash = false;
        // Reserved pages are 512-byte aligned, so only aligned positions inside
        // the candidate range can collide.
        const std::uint64_t first = cursor_ & ~(kPageSize - 1);
        for (std::uint64_t p = first; p < cursor_ + cb; p += kPageSize) {
            if (p + kPageSize <= cursor_) continue;
            if (isReservedPage(p)) {
                cursor_ = p + kPageSize;
                clash = true;
                break;
            }
        }
        if (!clash) break;
    }
    const std::uint64_t ib = cursor_;
    cursor_ += cb;
    markAllocated(ib, cb);
    return ib;
}

void NdbWriter::ensureAMapCount(std::size_t n) {
    while (amaps_.size() < n) {
        const std::size_t index = amaps_.size();
        amaps_.emplace_back(496, 0);
        const std::uint64_t amap_ib = kFirstAMapPos + index * kAMapSpan;
        last_amap_ib_ = amap_ib;
        // An AMap page accounts for itself.
        markAllocated(amap_ib, kPageSize);
        // ... and for the DList and any PMap page inside its span.
        if (index == 0) {
            markAllocated(kFirstPMapPos, kPageSize);
        } else {
            const std::uint64_t span_end = amap_ib + kAMapSpan;
            for (std::uint64_t p = kFirstPMapPos; p < span_end; p += kPMapSpan) {
                if (p >= amap_ib) markAllocated(p, kPageSize);
            }
        }
    }
}

void NdbWriter::markAllocated(std::uint64_t ib, std::uint64_t cb) {
    if (ib < kFirstAMapPos) return;
    const std::size_t map = static_cast<std::size_t>((ib - kFirstAMapPos) / kAMapSpan);
    ensureAMapCount(map + 1);
    const std::uint64_t base = kFirstAMapPos + static_cast<std::uint64_t>(map) * kAMapSpan;
    std::uint64_t first_bit = (ib - base) / kBlockAlign;
    std::uint64_t nbits = alignUp(cb, kBlockAlign) / kBlockAlign;
    auto& bits = amaps_[map];
    for (std::uint64_t i = 0; i < nbits; ++i) {
        const std::uint64_t bit = first_bit + i;
        if (bit / 8 >= bits.size()) break;  // spills into the next AMap span
        bits[static_cast<std::size_t>(bit / 8)] |=
            static_cast<std::uint8_t>(0x80u >> (bit % 8));
    }
}

void NdbWriter::writeAt(std::uint64_t ib, const void* data, std::size_t len) {
    out_.seekp(static_cast<std::streamoff>(ib), std::ios::beg);
    out_.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
    if (!out_) throw PstError("short write to PST file");
}

// ------------------------------------------------------------------- blocks

Bid NdbWriter::emitBlock(const void* data, std::size_t cb, bool internal) {
    if (cb > kMaxBlockData) throw PstError("block payload exceeds 8176 bytes");
    const std::uint64_t total = alignUp(cb + 16, kBlockAlign);
    const std::uint64_t ib = allocate(total);
    const Bid bid = makeBid(next_bid_index_++, internal);

    std::vector<std::uint8_t> buf(total, 0);
    if (cb) std::memcpy(buf.data(), data, cb);

    // BLOCKTRAILER occupies the last 16 bytes of the (padded) block.
    std::uint8_t* t = buf.data() + total - 16;
    poke16(t, static_cast<std::uint16_t>(cb));
    poke16(t + 2, computeSig(ib, bid));
    poke32(t + 4, computeCrc(data, cb));
    poke64(t + 8, bid);

    writeAt(ib, buf.data(), buf.size());
    bbt_.push_back({bid, ib, static_cast<std::uint16_t>(cb)});
    return bid;
}

Bid NdbWriter::emitXBlock(const std::vector<Bid>& children, std::uint8_t level,
                          std::uint32_t total_bytes) {
    std::vector<std::uint8_t> buf;
    buf.reserve(8 + children.size() * 8);
    put8(buf, 0x01);   // btype: XBLOCK / XXBLOCK
    put8(buf, level);  // 1 = XBLOCK, 2 = XXBLOCK
    put16(buf, static_cast<std::uint16_t>(children.size()));
    put32(buf, total_bytes);
    for (Bid b : children) put64(buf, b);
    return emitBlock(buf.data(), buf.size(), /*internal=*/true);
}

Bid NdbWriter::writeData(const void* data, std::size_t len) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    if (len <= kMaxBlockData) return emitBlock(p, len, /*internal=*/false);
    if (len > 0xFFFFFFFFull) throw PstError("data tree larger than 4 GiB");

    std::vector<Bid> leaves;
    for (std::size_t off = 0; off < len; off += kMaxBlockData) {
        const std::size_t n = std::min<std::size_t>(kMaxBlockData, len - off);
        leaves.push_back(emitBlock(p + off, n, /*internal=*/false));
    }
    const auto total = static_cast<std::uint32_t>(len);

    if (leaves.size() <= kXBlockFanout) return emitXBlock(leaves, 1, total);

    std::vector<Bid> xblocks;
    for (std::size_t i = 0; i < leaves.size(); i += kXBlockFanout) {
        const std::size_t n = std::min(kXBlockFanout, leaves.size() - i);
        std::vector<Bid> slice(leaves.begin() + static_cast<long>(i),
                               leaves.begin() + static_cast<long>(i + n));
        // Every child XBLOCK except the last is completely full.
        const auto covered = static_cast<std::uint32_t>(
            std::min<std::size_t>(n * kMaxBlockData, len - i * kMaxBlockData));
        xblocks.push_back(emitXBlock(slice, 1, covered));
    }
    if (xblocks.size() > kXBlockFanout) {
        throw PstError("data tree needs more than two levels; not supported");
    }
    return emitXBlock(xblocks, 2, total);
}

Bid NdbWriter::writeSubnodes(std::vector<SubnodeEntry> entries) {
    if (entries.empty()) return 0;
    std::sort(entries.begin(), entries.end(),
              [](const SubnodeEntry& a, const SubnodeEntry& b) { return a.nid < b.nid; });

    auto emit_sl = [&](std::size_t first, std::size_t count) {
        std::vector<std::uint8_t> buf;
        put8(buf, 0x02);  // btype: SLBLOCK / SIBLOCK
        put8(buf, 0x00);  // cLevel 0 -> leaf
        put16(buf, static_cast<std::uint16_t>(count));
        put32(buf, 0);    // dwPadding (Unicode only)
        for (std::size_t i = first; i < first + count; ++i) {
            put64(buf, entries[i].nid);
            put64(buf, entries[i].data);
            put64(buf, entries[i].sub);
        }
        return emitBlock(buf.data(), buf.size(), /*internal=*/true);
    };

    if (entries.size() <= kSlBlockFanout) return emit_sl(0, entries.size());

    struct Child { Nid key; Bid bid; };
    std::vector<Child> children;
    for (std::size_t i = 0; i < entries.size(); i += kSlBlockFanout) {
        const std::size_t n = std::min(kSlBlockFanout, entries.size() - i);
        children.push_back({entries[i].nid, emit_sl(i, n)});
    }
    if (children.size() > kSiBlockFanout) {
        throw PstError("subnode tree needs more than two levels; not supported");
    }
    std::vector<std::uint8_t> buf;
    put8(buf, 0x02);
    put8(buf, 0x01);  // cLevel 1 -> SIBLOCK
    put16(buf, static_cast<std::uint16_t>(children.size()));
    put32(buf, 0);
    for (const auto& c : children) {
        put64(buf, c.key);
        put64(buf, c.bid);
    }
    return emitBlock(buf.data(), buf.size(), /*internal=*/true);
}

void NdbWriter::addNode(Nid nid, Bid data, Bid sub, Nid parent) {
    nbt_.push_back({nid, data, sub, parent});
}

// ------------------------------------------------------------------ b-trees

NdbWriter::Bref NdbWriter::emitBTPage(const std::vector<RawEntry>& entries,
                                      std::size_t first, std::size_t count,
                                      std::uint8_t entry_size, std::uint8_t level,
                                      std::uint8_t page_type) {
    std::vector<std::uint8_t> page(kPageSize, 0);
    std::size_t off = 0;
    for (std::size_t i = first; i < first + count; ++i) {
        std::memcpy(page.data() + off, entries[i].bytes.data(), entry_size);
        off += entry_size;
    }
    page[488] = static_cast<std::uint8_t>(count);
    page[489] = static_cast<std::uint8_t>(entriesPerPage(entry_size));
    page[490] = entry_size;
    page[491] = level;
    // page[492..495] is dwPadding, already zero.

    const std::uint64_t ib = allocate(kPageSize);
    const Bid bid = makeBid(next_bid_index_++, /*internal=*/false);
    std::uint8_t* t = page.data() + 496;
    t[0] = page_type;
    t[1] = page_type;
    poke16(t + 2, computeSig(ib, bid));
    poke32(t + 4, computeCrc(page.data(), 496));
    poke64(t + 8, bid);

    writeAt(ib, page.data(), page.size());
    return Bref{bid, ib};
}

NdbWriter::Bref NdbWriter::buildBTree(std::uint8_t page_type,
                                      std::vector<RawEntry> entries,
                                      std::uint8_t entry_size) {
    std::sort(entries.begin(), entries.end(),
              [](const RawEntry& a, const RawEntry& b) { return a.key < b.key; });

    // An empty tree is still one (empty) leaf page: readers expect a root.
    if (entries.empty()) {
        return emitBTPage(entries, 0, 0, entry_size, 0, page_type);
    }

    std::vector<RawEntry> level_entries;
    const std::size_t per_leaf = entriesPerPage(entry_size);
    for (std::size_t i = 0; i < entries.size(); i += per_leaf) {
        const std::size_t n = std::min(per_leaf, entries.size() - i);
        const Bref child = emitBTPage(entries, i, n, entry_size, 0, page_type);
        RawEntry e;
        e.key = entries[i].key;
        put64(e.bytes, e.key);        // BTENTRY.btkey
        put64(e.bytes, child.bid);    // BTENTRY.BREF.bid
        put64(e.bytes, child.ib);     // BTENTRY.BREF.ib
        level_entries.push_back(std::move(e));
    }

    std::uint8_t level = 1;
    const std::size_t per_branch = entriesPerPage(kSizeBTEntry);
    while (level_entries.size() > 1) {
        std::vector<RawEntry> next;
        for (std::size_t i = 0; i < level_entries.size(); i += per_branch) {
            const std::size_t n = std::min(per_branch, level_entries.size() - i);
            const Bref child =
                emitBTPage(level_entries, i, n, kSizeBTEntry, level, page_type);
            RawEntry e;
            e.key = level_entries[i].key;
            put64(e.bytes, e.key);
            put64(e.bytes, child.bid);
            put64(e.bytes, child.ib);
            next.push_back(std::move(e));
        }
        level_entries = std::move(next);
        ++level;
        if (level > 8) throw PstError("B-tree grew beyond 8 levels");
    }

    // The root's BREF is embedded in the single remaining entry.
    const std::uint8_t* p = level_entries.front().bytes.data();
    return Bref{peek64(p + 8), peek64(p + 16)};
}

// ------------------------------------------------------------- fixed pages

void NdbWriter::emitFixedPages() {
    auto write_page = [&](std::uint64_t ib, std::uint8_t ptype,
                          const std::vector<std::uint8_t>& body, bool signed_page) {
        std::vector<std::uint8_t> page(kPageSize, 0);
        std::memcpy(page.data(), body.data(), std::min<std::size_t>(496, body.size()));
        const Bid bid = makeBid(next_bid_index_++, /*internal=*/false);
        std::uint8_t* t = page.data() + 496;
        t[0] = ptype;
        t[1] = ptype;
        poke16(t + 2, signed_page ? computeSig(ib, bid) : 0);
        poke32(t + 4, computeCrc(page.data(), 496));
        poke64(t + 8, bid);
        writeAt(ib, page.data(), page.size());
    };

    // Density list: present but empty.  It is only an allocation hint.
    {
        std::vector<std::uint8_t> body(496, 0);
        write_page(kDListPos, kPTypeDList, body, /*signed_page=*/true);
    }
    // Allocation maps.
    for (std::size_t i = 0; i < amaps_.size(); ++i) {
        write_page(kFirstAMapPos + i * kAMapSpan, kPTypeAMap, amaps_[i],
                   /*signed_page=*/false);
    }
    // Page maps.  Deprecated by [MS-PST] but still expected to exist; mark
    // everything allocated so no reader tries to reuse the space.
    for (std::uint64_t ib = kFirstPMapPos; ib < cursor_; ib += kPMapSpan) {
        std::vector<std::uint8_t> body(496, 0xFF);
        write_page(ib, kPTypePMap, body, /*signed_page=*/false);
    }
}

void NdbWriter::writeHeader(const Bref& nbt, const Bref& bbt) {
    std::vector<std::uint8_t> h(kHeaderSize, 0);

    poke32(h.data() + 0, 0x4E444221);  // "!BDN"
    // h[4..7] dwCRCPartial, filled in below.
    poke16(h.data() + 8, 0x4D53);      // wMagicClient: the bytes "SM"
    poke16(h.data() + 10, 23);         // wVer: 23 == Unicode
    poke16(h.data() + 12, 19);         // wVerClient
    h[14] = 0x01;                      // bPlatformCreate
    h[15] = 0x01;                      // bPlatformAccess
    // h[16..23] reserved
    poke64(h.data() + 24, 0);          // bidUnused
    const Bid next_bid = makeBid(next_bid_index_, false);
    poke64(h.data() + 32, next_bid);   // bidNextP
    poke32(h.data() + 40, 1);          // dwUnique

    // rgnid[32]: next free NID index per node type.  Readers treat this as a
    // hint; we publish one past the highest index we actually used.
    std::array<std::uint32_t, 32> next_nid{};
    for (std::size_t i = 0; i < next_nid.size(); ++i) {
        next_nid[i] = makeNid(static_cast<NidType>(i), 1);
    }
    for (const auto& n : nbt_) {
        const std::size_t t = nidType(n.nid);
        const Nid candidate = makeNid(static_cast<NidType>(t), nidIndex(n.nid) + 1);
        if (candidate > next_nid[t]) next_nid[t] = candidate;
    }
    for (std::size_t i = 0; i < next_nid.size(); ++i) {
        poke32(h.data() + 44 + i * 4, next_nid[i]);
    }
    // h[172..179] qwUnused

    // ROOT, [MS-PST] 2.2.2.5.
    std::uint8_t* root = h.data() + 180;
    poke32(root + 0, 0);                  // dwReserved
    poke64(root + 4, cursor_);            // ibFileEof
    poke64(root + 12, last_amap_ib_);     // ibAMapLast
    poke64(root + 20, 0);                 // cbAMapFree
    poke64(root + 28, 0);                 // cbPMapFree
    poke64(root + 36, nbt.bid);           // BREFNBT
    poke64(root + 44, nbt.ib);
    poke64(root + 52, bbt.bid);           // BREFBBT
    poke64(root + 60, bbt.ib);
    root[68] = 0x02;                      // fAMapValid = VALID2
    root[69] = 0x00;                      // bReserved
    poke16(root + 70, 0);                 // wReserved

    // FMap and FPMap are unused in Unicode PSTs; 0xFF means "not present".
    std::memset(h.data() + 256, 0xFF, 128);  // rgbFM
    std::memset(h.data() + 384, 0xFF, 128);  // rgbFP

    h[512] = 0x80;  // bSentinel
    h[513] = 0x00;  // bCryptMethod: NDB_CRYPT_NONE
    poke64(h.data() + 516, next_bid);  // bidNextB

    // dwCRCPartial covers 471 bytes from wMagicClient; dwCRCFull covers 516.
    poke32(h.data() + 524, computeCrc(h.data() + 8, 516));
    poke32(h.data() + 4, computeCrc(h.data() + 8, 471));

    writeAt(0, h.data(), h.size());
}

void NdbWriter::finish() {
    if (finished_) return;
    finished_ = true;

    std::vector<RawEntry> bbt_entries;
    bbt_entries.reserve(bbt_.size());
    for (const auto& e : bbt_) {
        RawEntry r;
        r.key = e.bid;
        put64(r.bytes, e.bid);
        put64(r.bytes, e.ib);
        put16(r.bytes, e.cb);
        put16(r.bytes, 2);  // cRef: one reference plus the implicit one
        put32(r.bytes, 0);  // dwPadding
        bbt_entries.push_back(std::move(r));
    }

    std::vector<RawEntry> nbt_entries;
    nbt_entries.reserve(nbt_.size());
    for (const auto& e : nbt_) {
        RawEntry r;
        r.key = e.nid;
        put64(r.bytes, e.nid);
        put64(r.bytes, e.data);
        put64(r.bytes, e.sub);
        put32(r.bytes, e.parent);
        put32(r.bytes, 0);  // dwPadding
        nbt_entries.push_back(std::move(r));
    }

    // The NBT is built first so that its pages are already accounted for in the
    // allocation maps before the BBT pages are laid out.  Neither tree indexes
    // pages, so building them does not feed back into their own contents.
    const Bref nbt_root = buildBTree(kPTypeNBT, std::move(nbt_entries), kSizeNBTEntry);
    const Bref bbt_root = buildBTree(kPTypeBBT, std::move(bbt_entries), kSizeBBTEntry);

    emitFixedPages();
    writeHeader(nbt_root, bbt_root);

    out_.flush();
    if (!out_) throw PstError("failed flushing PST file");
    out_.close();
}

}  // namespace imap2pst::pst
