#pragma once

// On-disk constants and small helpers for the Unicode PST format.
//
// Reference: [MS-PST] "Outlook Personal Folders (.pst) File Format", revision
// 4.0.  Section numbers in comments below refer to that document.  Ambiguities
// were resolved against java-libpst (github.com/rjohnsondev/java-libpst).
//
// This writer only ever emits Unicode (64-bit) PSTs with NDB_CRYPT_NONE.  The
// ANSI layout, the permute/cyclic encodings and FMap/FPMap pages are all out of
// scope; see README "Known limitations".

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace imap2pst::pst {

using Nid = std::uint32_t;
using Bid = std::uint64_t;
using Hid = std::uint32_t;
using Hnid = std::uint32_t;

// ---------------------------------------------------------------- geometry
inline constexpr std::uint64_t kPageSize      = 512;
inline constexpr std::uint64_t kBlockAlign    = 64;
inline constexpr std::uint64_t kMaxBlockData  = 8176;   // 8192 - BLOCKTRAILER
inline constexpr std::uint64_t kHeaderSize    = 564;    // Unicode HEADER

// Fixed page positions, [MS-PST] 2.2.2.7.
inline constexpr std::uint64_t kDListPos      = 0x4200;
inline constexpr std::uint64_t kFirstAMapPos  = 0x4400;
inline constexpr std::uint64_t kFirstPMapPos  = 0x4600;
inline constexpr std::uint64_t kFirstDataPos  = 0x4800;
// One AMap page maps 496 * 8 * 64 bytes.
inline constexpr std::uint64_t kAMapSpan      = 496ull * 8 * 64;      // 0x3E000
// One PMap page maps 496 * 8 * 512 bytes.
inline constexpr std::uint64_t kPMapSpan      = 496ull * 8 * 512;     // 0x1F0000

// ------------------------------------------------------------- page types
enum PageType : std::uint8_t {
    kPTypeBBT   = 0x80,
    kPTypeNBT   = 0x81,
    kPTypeFMap  = 0x82,
    kPTypePMap  = 0x83,
    kPTypeAMap  = 0x84,
    kPTypeFPMap = 0x85,
    kPTypeDList = 0x86,
};

// Entry sizes for Unicode B-tree pages, [MS-PST] 2.2.2.7.7.
inline constexpr std::uint8_t kSizeBTEntry  = 24;  // internal
inline constexpr std::uint8_t kSizeBBTEntry = 24;  // BBT leaf
inline constexpr std::uint8_t kSizeNBTEntry = 32;  // NBT leaf
inline constexpr std::uint16_t kBTPageDataSize = 488;

// -------------------------------------------------------------- NID types
enum NidType : std::uint8_t {
    kNidTypeHid                  = 0x00,
    kNidTypeInternal             = 0x01,
    kNidTypeNormalFolder         = 0x02,
    kNidTypeSearchFolder         = 0x03,
    kNidTypeNormalMessage        = 0x04,
    kNidTypeAttachment           = 0x05,
    kNidTypeSearchUpdateQueue    = 0x06,
    kNidTypeSearchCriteria       = 0x07,
    kNidTypeAssocMessage         = 0x08,
    kNidTypeContentsTableIndex   = 0x0A,
    kNidTypeReceiveFolderTable   = 0x0B,
    kNidTypeOutgoingQueueTable   = 0x0C,
    kNidTypeHierarchyTable       = 0x0D,
    kNidTypeContentsTable        = 0x0E,
    kNidTypeAssocContentsTable   = 0x0F,
    kNidTypeSearchContentsTable  = 0x10,
    kNidTypeAttachmentTable      = 0x11,
    kNidTypeRecipientTable       = 0x12,
    kNidTypeSearchTableIndex     = 0x13,
    kNidTypeLtp                  = 0x1F,
};

constexpr Nid makeNid(NidType type, std::uint32_t index) {
    return (index << 5) | (type & 0x1F);
}
constexpr NidType nidType(Nid nid) {
    return static_cast<NidType>(nid & 0x1F);
}
constexpr std::uint32_t nidIndex(Nid nid) { return nid >> 5; }

// NID indexes below this are reserved for the nodes the format itself defines;
// objects a writer creates start here.  Outlook's own recovery assigns folders
// from this point (a rebuilt folder comes back as NID 0x8022, index 0x401).
inline constexpr std::uint32_t kFirstUserNidIndex = 0x400;

// Reserved node identifiers, [MS-PST] 2.4.1.
inline constexpr Nid kNidMessageStore        = 0x21;
inline constexpr Nid kNidNameToIdMap         = 0x61;
inline constexpr Nid kNidNormalFolderTemplate= 0xA1;
inline constexpr Nid kNidRootFolder          = 0x122;
inline constexpr Nid kNidSearchManagementQ   = 0x1E1;
inline constexpr Nid kNidSearchActivityList  = 0x201;
// Subnode identifiers used inside a message node.
inline constexpr Nid kNidRecipientTable      = 0x692;
inline constexpr Nid kNidAttachmentTable     = 0x671;

// --------------------------------------------------------------------- BID
// b0 is reserved and b1 marks an "internal" block (XBLOCK/XXBLOCK/SL/SI).
constexpr Bid makeBid(std::uint64_t index, bool internal) {
    return (index << 2) | (internal ? 2u : 0u);
}
constexpr bool bidIsInternal(Bid bid) { return (bid & 2) != 0; }

// --------------------------------------------------------------------- HID
// hidType: 5 bits, hidIndex: 11 bits (1-based), hidBlockIndex: 16 bits.
constexpr Hid makeHid(std::uint16_t block_index, std::uint16_t alloc_index) {
    return (static_cast<std::uint32_t>(block_index) << 16) |
           (static_cast<std::uint32_t>(alloc_index) << 5);
}
constexpr std::uint16_t hidAllocIndex(Hid hid) { return (hid >> 5) & 0x7FF; }
constexpr std::uint16_t hidBlockIndex(Hid hid) { return hid >> 16; }
// An HNID holds an HID when its low 5 bits are zero, otherwise a NID.
constexpr bool hnidIsHid(Hnid v) { return (v & 0x1F) == 0; }

// ----------------------------------------------------------- HN client sigs
enum HeapClientSig : std::uint8_t {
    kHnSigTC  = 0x7C,
    kHnSigBTH = 0xB5,
    kHnSigPC  = 0xBC,
};
inline constexpr std::uint8_t kHnSignature = 0xEC;  // HNHDR.bSig

// ---------------------------------------------------------- property types
enum PropType : std::uint16_t {
    kPtUnspecified = 0x0000,
    kPtNull        = 0x0001,
    kPtShort       = 0x0002,  // PT_I2
    kPtLong        = 0x0003,  // PT_LONG
    kPtFloating    = 0x0004,
    kPtDouble      = 0x0005,
    kPtCurrency    = 0x0006,
    kPtAppTime     = 0x0007,
    kPtError       = 0x000A,
    kPtBoolean     = 0x000B,
    kPtObject      = 0x000D,
    kPtLongLong    = 0x0014,  // PT_I8
    kPtUnicode     = 0x001F,
    kPtString8     = 0x001E,
    kPtSystime     = 0x0040,
    kPtGuid        = 0x0048,
    kPtBinary      = 0x0102,
    kPtMvLong      = 0x1003,
    kPtMvUnicode   = 0x101F,
    kPtMvBinary    = 0x1102,
};

// Number of bytes a fixed-width property occupies, or 0 when variable.
constexpr std::size_t fixedPropSize(PropType t) {
    switch (t) {
        case kPtShort:    return 2;
        case kPtBoolean:  return 1;
        case kPtLong:
        case kPtFloating:
        case kPtError:    return 4;
        case kPtDouble:
        case kPtCurrency:
        case kPtAppTime:
        case kPtLongLong:
        case kPtSystime:  return 8;
        default:          return 0;
    }
}
// Values of four bytes or fewer live directly in the record; anything larger
// is referenced through an HNID.  [MS-PST] 2.3.3.3.
constexpr bool propIsInline(PropType t) {
    const std::size_t n = fixedPropSize(t);
    return n != 0 && n <= 4;
}

// -------------------------------------------------------- byte-order helpers
inline void put8(std::vector<std::uint8_t>& b, std::uint8_t v) { b.push_back(v); }
inline void put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
inline void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
inline void put64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
inline void putBytes(std::vector<std::uint8_t>& b, const void* p, std::size_t n) {
    const auto* q = static_cast<const std::uint8_t*>(p);
    b.insert(b.end(), q, q + n);
}
inline void poke16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void poke32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline void poke64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline std::uint16_t peek16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
inline std::uint32_t peek32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline std::uint64_t peek64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

constexpr std::uint64_t alignUp(std::uint64_t v, std::uint64_t a) {
    return (v + a - 1) / a * a;
}

// UTF-8 -> UTF-16LE, the on-disk form of every PT_UNICODE value.
std::vector<std::uint8_t> utf8ToUtf16le(const std::string& s);
// Unix seconds -> FILETIME (100ns ticks since 1601-01-01).
std::uint64_t unixToFiletime(std::int64_t unix_seconds);

// [MS-PST] 5.1: page and block signature.  Both arguments are truncated to
// 32 bits before mixing, which is what the reference implementation does.
inline std::uint16_t computeSig(std::uint64_t ib, std::uint64_t bid) {
    std::uint32_t v = static_cast<std::uint32_t>(bid) ^ static_cast<std::uint32_t>(ib);
    return static_cast<std::uint16_t>((v >> 16) ^ v);
}

}  // namespace imap2pst::pst
