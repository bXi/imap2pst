#pragma once

// NDB (Node Database) layer: the raw file, its allocation maps, the block
// store and the two B-trees that index them.
//
// The writer is append-only.  Space is handed out by a bump allocator that
// steps over the fixed AMap/PMap/DList page positions, and nothing is ever
// freed or reused -- see README "Known limitations".  Blocks are emitted as
// soon as they are handed to the writer; the NBT and BBT pages, the allocation
// maps and the header are all written by finish() once every block is known.

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "pst/pst_format.h"

namespace imap2pst::pst {

class PstError : public std::runtime_error {
 public:
    using std::runtime_error::runtime_error;
};

// One entry of a node's subnode B-tree.
struct SubnodeEntry {
    Nid nid = 0;
    Bid data = 0;
    Bid sub = 0;
};

class NdbWriter {
 public:
    explicit NdbWriter(const std::string& path);
    ~NdbWriter();

    NdbWriter(const NdbWriter&) = delete;
    NdbWriter& operator=(const NdbWriter&) = delete;

    // Stores `len` bytes as a data tree and returns the BID of its root, which
    // is a plain data block, an XBLOCK or an XXBLOCK depending on size.
    Bid writeData(const void* data, std::size_t len);
    Bid writeData(const std::vector<std::uint8_t>& v) { return writeData(v.data(), v.size()); }

    // The two halves of writeData, exposed so callers that produce their data
    // incrementally can hand over one chunk at a time and never hold the whole
    // stream in memory.  Chunks must be exactly kMaxBlockData bytes except the
    // last.  assembleDataTree() takes the leaf BIDs in stream order.
    Bid writeLeafChunk(const void* data, std::size_t len);
    Bid assembleDataTree(const std::vector<Bid>& leaves, std::uint64_t total_bytes);

    // Stores a subnode B-tree.  Returns 0 for an empty entry list, which is the
    // encoding for "this node has no subnodes".
    Bid writeSubnodes(std::vector<SubnodeEntry> entries);

    // Registers a top-level node in the NBT.  `sub` and `parent` may be 0.
    void addNode(Nid nid, Bid data, Bid sub, Nid parent);

    // Writes the B-tree pages, allocation maps and header, then closes.
    void finish();

    std::uint64_t fileSize() const { return cursor_; }

 private:
    struct BbtEntry {
        Bid bid;
        std::uint64_t ib;
        std::uint16_t cb;
    };
    struct NbtEntry {
        Nid nid;
        Bid data;
        Bid sub;
        Nid parent;
    };
    struct Bref {
        Bid bid = 0;
        std::uint64_t ib = 0;
    };

    // --- space management ---------------------------------------------------
    static bool isReservedPage(std::uint64_t ib);
    std::uint64_t allocate(std::uint64_t cb);
    // Pages must start on a 512-byte boundary.  Blocks only need 64-byte
    // alignment, so the cursor is usually somewhere in between.
    std::uint64_t allocatePage();
    void markAllocated(std::uint64_t ib, std::uint64_t cb);
    void ensureAMapCount(std::size_t n);

    // --- raw io -------------------------------------------------------------
    void writeAt(std::uint64_t ib, const void* data, std::size_t len);

    // --- block emission -----------------------------------------------------
    Bid emitBlock(const void* data, std::size_t cb, bool internal);
    Bid emitXBlock(const std::vector<Bid>& children, std::uint8_t level,
                   std::uint32_t total_bytes);

    // --- b-tree construction ------------------------------------------------
    // Leaf entries are serialized straight out of the POD bbt_/nbt_ vectors by
    // a callback, so a mailbox-sized tree never materializes a second copy of
    // every entry just to build its pages.
    struct BranchEntry {
        std::uint64_t key;
        Bid bid;
        std::uint64_t ib;
    };
    using KeyFn = std::uint64_t (*)(const void* base, std::size_t index);
    using EntryFn = void (*)(const void* base, std::size_t index, std::uint8_t* dst);

    Bref buildBTree(std::uint8_t page_type, std::uint8_t entry_size,
                    std::size_t count, const void* base, KeyFn key_of,
                    EntryFn write_entry);
    Bref emitLeafPage(std::uint8_t page_type, std::uint8_t entry_size,
                      const void* base, std::size_t first, std::size_t count,
                      EntryFn write_entry);
    Bref emitBranchPage(std::uint8_t page_type, const std::vector<BranchEntry>& entries,
                        std::size_t first, std::size_t count, std::uint8_t level);
    Bref finishBTPage(std::vector<std::uint8_t>& page, std::uint8_t page_type,
                      std::uint8_t entry_size, std::size_t count, std::uint8_t level);
    void emitFixedPages();
    void writeHeader(const Bref& nbt, const Bref& bbt);

    std::string path_;
    std::fstream out_;
    std::uint64_t cursor_ = kFirstDataPos;
    std::uint64_t next_bid_index_ = 1;
    bool finished_ = false;

    std::vector<BbtEntry> bbt_;
    std::vector<NbtEntry> nbt_;
    std::vector<std::vector<std::uint8_t>> amaps_;  // 496-byte bitmaps
    std::uint64_t last_amap_ib_ = kFirstAMapPos;
    std::uint64_t file_end_ = kFirstDataPos;
    std::uint64_t amap_free_ = 0;
    std::uint64_t pmap_free_ = 0;
};

}  // namespace imap2pst::pst
