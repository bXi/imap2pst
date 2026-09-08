#pragma once

// The public face of the PST module.
//
// Callers create folders, drop normalized Messages into them and call finish().
// Everything below -- NDB blocks, heaps, property and table contexts, the
// name-to-id map -- is an implementation detail of this class.

#include <memory>
#include <string>
#include <vector>

#include "core/message.h"
#include "pst/ltp.h"
#include "pst/ndb.h"
#include "pst/nameid_map.h"
#include "pst/streaming_table.h"

namespace imap2pst::pst {

// Identifies a folder inside the PST being written.  It is the folder's NID.
using FolderId = Nid;

class PstWriter {
 public:
    explicit PstWriter(const std::string& path);
    ~PstWriter();

    PstWriter(const PstWriter&) = delete;
    PstWriter& operator=(const PstWriter&) = delete;

    // "Top of Personal Folders": the folder user-visible content hangs off.
    FolderId ipmSubtree() const { return ipm_subtree_; }

    FolderId createFolder(FolderId parent, const std::string& name);

    // Writes the message and links it into `folder`'s contents table.
    void addMessage(FolderId folder, const Message& msg);

    // Flushes every remaining structure and closes the file.  Safe to call once.
    void finish();

 private:
    struct Folder {
        Nid nid = 0;
        Nid parent = 0;
        std::string name;
        std::vector<Nid> children;
        // Streamed rather than buffered: a folder's contents table is the one
        // structure here that scales with mailbox size, and holding it whole
        // costs roughly a kilobyte per message until finish().
        std::unique_ptr<TableContextWriter> contents;
        std::size_t message_count = 0;
        std::uint32_t unread = 0;
    };

    Folder& folder(FolderId id);
    FolderId makeFolder(Nid parent, const std::string& name, Nid forced_nid);
    std::vector<std::uint8_t> entryId(Nid nid) const;
    void writeNodeFromHeap(Nid nid, Nid parent, const std::vector<std::uint8_t>& heap,
                           SubnodeAllocator& subs);
    void writeMessageStore();
    void writeNameIdMap();
    void writeFolders();
    void applyHeaderProperties(PropertyContext& pc, const Message& msg);

    // The column sets Outlook requires each kind of folder table to declare.
    static std::vector<PropTag> contentsColumns();
    static std::vector<PropTag> hierarchyColumns();
    static std::vector<PropTag> associatedColumns();

    NdbWriter ndb_;
    NameIdMap names_;
    std::vector<Folder> folders_;
    std::uint8_t store_guid_[16] = {};
    Nid root_folder_ = kNidRootFolder;
    Nid ipm_subtree_ = 0;
    std::uint32_t next_folder_index_ = kFirstUserNidIndex;
    std::uint32_t next_message_index_ = kFirstUserNidIndex;
    bool finished_ = false;
};

}  // namespace imap2pst::pst
