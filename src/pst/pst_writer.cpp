#include "pst/pst_writer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <map>
#include <random>

namespace imap2pst::pst {
namespace {

std::string joinMailboxes(const std::vector<Mailbox>& v) {
    std::string out;
    for (const auto& m : v) {
        if (!out.empty()) out += "; ";
        out += m.name.empty() ? m.email : m.name;
    }
    return out;
}

std::int64_t nowUnix() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// Headers that already have a structured MAPI property; they still appear in
// PidTagTransportMessageHeaders, they just do not also get a named property.
bool isStructuredHeader(const std::string& name) {
    static const char* kKnown[] = {"from", "to",   "cc",         "bcc",
                                   "reply-to", "subject", "date", "message-id",
                                   "in-reply-to"};
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* k : kKnown) {
        if (lower == k) return true;
    }
    return false;
}

}  // namespace

PstWriter::PstWriter(const std::string& path) : ndb_(path) {
    std::random_device rd;
    for (auto& b : store_guid_) b = static_cast<std::uint8_t>(rd() & 0xFF);

    // The root node of the folder tree, then the subtree everything the user
    // can see hangs off.
    makeFolder(0, "Root - Mailbox", kNidRootFolder);
    ipm_subtree_ = makeFolder(kNidRootFolder, "Top of Personal Folders", 0);
}

PstWriter::~PstWriter() {
    try {
        finish();
    } catch (...) {
        // A destructor must not propagate; callers who care call finish().
    }
}

PstWriter::Folder& PstWriter::folder(FolderId id) {
    for (auto& f : folders_) {
        if (f.nid == id) return f;
    }
    throw PstError("unknown folder id " + std::to_string(id));
}

FolderId PstWriter::makeFolder(Nid parent, const std::string& name, Nid forced_nid) {
    Folder f;
    f.nid = forced_nid ? forced_nid
                       : makeNid(kNidTypeNormalFolder, next_folder_index_++);
    f.parent = parent;
    f.name = name;
    f.contents = std::make_unique<TableContextWriter>(
        ndb_, std::vector<PropTag>{PR_SUBJECT, PR_SENDER_NAME, PR_DISPLAY_TO,
                                   PR_MESSAGE_CLASS, PR_MESSAGE_DELIVERY_TIME,
                                   PR_MESSAGE_FLAGS, PR_MESSAGE_SIZE, PR_HASATTACH});
    folders_.push_back(std::move(f));

    if (parent) {
        for (auto& p : folders_) {
            if (p.nid == parent) {
                p.children.push_back(folders_.back().nid);
                break;
            }
        }
    }
    return folders_.back().nid;
}

FolderId PstWriter::createFolder(FolderId parent, const std::string& name) {
    folder(parent);  // validates
    return makeFolder(parent, name, 0);
}

std::vector<std::uint8_t> PstWriter::entryId(Nid nid) const {
    std::vector<std::uint8_t> e;
    put32(e, 0);  // rgbFlags
    putBytes(e, store_guid_, 16);
    put32(e, nid);
    return e;
}

void PstWriter::writeNodeFromHeap(Nid nid, Nid parent,
                                  const std::vector<std::uint8_t>& heap,
                                  SubnodeAllocator& subs) {
    const Bid data = ndb_.writeData(heap);
    const Bid sub = ndb_.writeSubnodes(subs.entries());
    ndb_.addNode(nid, data, sub, parent);
}

// ------------------------------------------------------------------ messages

void PstWriter::applyHeaderProperties(PropertyContext& pc, const Message& msg) {
    // The catch-all: every header, in order, exactly as received.
    const std::string blob = msg.header_blob();
    if (!blob.empty()) pc.setString(PR_TRANSPORT_MESSAGE_HEADERS, blob);

    // Anything without a structured slot also becomes a named property under
    // PS_INTERNET_HEADERS so it survives as an addressable value, not just as
    // text inside the blob.  Repeated headers are joined with newlines.
    std::vector<std::string> order;
    std::map<std::string, std::string> merged;
    for (const auto& h : msg.headers) {
        if (isStructuredHeader(h.name)) continue;
        auto it = merged.find(h.name);
        if (it == merged.end()) {
            merged.emplace(h.name, h.value);
            order.push_back(h.name);
        } else {
            it->second += "\n";
            it->second += h.value;
        }
    }
    for (const auto& name : order) {
        const std::uint16_t id = names_.idForString(kPsInternetHeaders, name);
        if (id == 0) break;  // name-to-id map is full; the blob still has it
        pc.setString(makeTag(id, kPtUnicode), merged[name]);
    }
}

void PstWriter::addMessage(FolderId folder_id, const Message& msg) {
    Folder& f = folder(folder_id);
    const Nid nid = makeNid(kNidTypeNormalMessage, next_message_index_++);

    SubnodeAllocator subs(ndb_);
    PropertyContext pc;

    pc.setString(PR_MESSAGE_CLASS, "IPM.Note");
    pc.setString(PR_SUBJECT, msg.subject);
    pc.setString(PR_CONVERSATION_TOPIC, msg.subject);
    if (!msg.body_text.empty()) pc.setString(PR_BODY, msg.body_text);
    if (!msg.body_html.empty()) {
        pc.setBinary(PR_HTML, msg.body_html.data(), msg.body_html.size());
    }
    // Every string this writer emits has been transcoded to UTF-8 by the MIME
    // layer, so the codepage is constant.
    pc.setInt32(PR_INTERNET_CPID, CP_UTF8);
    pc.setInt32(PR_MESSAGE_CODEPAGE, CP_UTF8);

    if (!msg.from.email.empty() || !msg.from.name.empty()) {
        pc.setString(PR_SENDER_NAME, msg.from.name.empty() ? msg.from.email : msg.from.name);
        pc.setString(PR_SENDER_EMAIL_ADDRESS, msg.from.email);
        pc.setString(PR_SENDER_ADDRTYPE, "SMTP");
        pc.setString(PR_SENT_REPRESENTING_NAME,
                     msg.from.name.empty() ? msg.from.email : msg.from.name);
    }
    pc.setString(PR_DISPLAY_TO, joinMailboxes(msg.to));
    pc.setString(PR_DISPLAY_CC, joinMailboxes(msg.cc));
    pc.setString(PR_DISPLAY_BCC, joinMailboxes(msg.bcc));

    const std::int64_t delivered = msg.delivery_time ? msg.delivery_time : msg.date;
    const std::int64_t sent = msg.date ? msg.date : msg.delivery_time;
    pc.setTime(PR_MESSAGE_DELIVERY_TIME, unixToFiletime(delivered));
    pc.setTime(PR_CLIENT_SUBMIT_TIME, unixToFiletime(sent));
    pc.setTime(PR_CREATION_TIME, unixToFiletime(delivered ? delivered : nowUnix()));
    pc.setTime(PR_LAST_MODIFICATION_TIME, unixToFiletime(delivered ? delivered : nowUnix()));

    std::uint32_t flags = MSGFLAG_UNMODIFIED;
    if (msg.seen()) flags |= MSGFLAG_READ;
    if (!msg.attachments.empty()) flags |= MSGFLAG_HASATTACH;
    pc.setInt32(PR_MESSAGE_FLAGS, flags);
    pc.setInt32(PR_FLAG_STATUS,
                (msg.flags & kFlagFlagged) ? FLAG_STATUS_FLAGGED : FLAG_STATUS_NONE);
    pc.setBool(PR_HASATTACH, !msg.attachments.empty());

    const std::uint32_t size =
        msg.size ? msg.size
                 : static_cast<std::uint32_t>(msg.body_text.size() + msg.body_html.size());
    pc.setInt32(PR_MESSAGE_SIZE, size);
    if (!msg.message_id.empty()) pc.setString(PR_INTERNET_MESSAGE_ID, msg.message_id);
    if (!msg.in_reply_to.empty()) pc.setString(PR_IN_REPLY_TO_ID, msg.in_reply_to);
    pc.setBinary(PR_ENTRYID, entryId(nid));

    applyHeaderProperties(pc, msg);

    // ---- recipient table -------------------------------------------------
    {
        TableContext rt;
        rt.addColumn(PR_RECIPIENT_TYPE);
        rt.addColumn(PR_DISPLAY_NAME);
        rt.addColumn(PR_EMAIL_ADDRESS);
        rt.addColumn(PR_ADDRTYPE);
        rt.addColumn(PR_OBJECT_TYPE);
        std::uint32_t row_id = 0;
        auto add = [&](const Mailbox& m, std::uint32_t type) {
            const std::size_t r = rt.addRow(row_id++);
            rt.setInt32(r, PR_RECIPIENT_TYPE, type);
            rt.setString(r, PR_DISPLAY_NAME, m.name.empty() ? m.email : m.name);
            rt.setString(r, PR_EMAIL_ADDRESS, m.email);
            rt.setString(r, PR_ADDRTYPE, "SMTP");
            rt.setInt32(r, PR_OBJECT_TYPE, MAPI_MAILUSER);
        };
        for (const auto& m : msg.to) add(m, MAPI_TO);
        for (const auto& m : msg.cc) add(m, MAPI_CC);
        for (const auto& m : msg.bcc) add(m, MAPI_BCC);
        SubnodeAllocator rt_subs(ndb_);
        const auto heap = rt.serialize(rt_subs);
        const Bid data = ndb_.writeData(heap);
        const Bid sub = ndb_.writeSubnodes(rt_subs.entries());
        subs.add(kNidRecipientTable, data, sub);
    }

    // ---- attachments -----------------------------------------------------
    // The attachment table is written even when the message has none.  A
    // message always has a recipient-table subnode, so its subnode tree always
    // exists, and libpff's lookup of a missing identifier inside an existing
    // subnode tree reports "found" with a null node rather than "not found" --
    // an empty table keeps that lookup on the happy path.
    {
        TableContext at;
        at.addColumn(PR_DISPLAY_NAME);
        at.addColumn(PR_ATTACH_LONG_FILENAME);
        at.addColumn(PR_ATTACH_FILENAME);
        at.addColumn(PR_ATTACH_METHOD);
        at.addColumn(PR_ATTACH_SIZE);
        at.addColumn(PR_ATTACH_MIME_TAG);
        at.addColumn(PR_ATTACH_NUM);

        for (std::size_t i = 0; i < msg.attachments.size(); ++i) {
            const Attachment& a = msg.attachments[i];
            const Nid att_nid = makeNid(kNidTypeAttachment, static_cast<std::uint32_t>(i + 1));
            const std::string name = a.filename.empty()
                                         ? ("attachment" + std::to_string(i + 1))
                                         : a.filename;

            SubnodeAllocator att_subs(ndb_);
            PropertyContext apc;
            apc.setInt32(PR_ATTACH_METHOD, ATTACH_BY_VALUE);
            apc.setInt32(PR_ATTACH_NUM, static_cast<std::uint32_t>(i));
            apc.setInt32(PR_ATTACH_RENDERING_POS, 0xFFFFFFFFu);
            apc.setInt32(PR_OBJECT_TYPE, MAPI_ATTACH);
            apc.setString(PR_DISPLAY_NAME, name);
            apc.setString(PR_ATTACH_FILENAME, name);
            apc.setString(PR_ATTACH_LONG_FILENAME, name);
            if (!a.content_type.empty()) apc.setString(PR_ATTACH_MIME_TAG, a.content_type);
            if (!a.content_id.empty()) apc.setString(PR_ATTACH_CONTENT_ID, a.content_id);
            apc.setInt32(PR_ATTACH_SIZE, static_cast<std::uint32_t>(a.data.size()));
            // Written straight through to its own subnode rather than copied
            // into the property context first: an attachment is the largest
            // thing this writer handles and does not need to exist twice.
            apc.setSpilledValue(PR_ATTACH_DATA_BIN,
                                att_subs.spill(a.data.data(), a.data.size()));

            const auto aheap = apc.serialize(att_subs);
            const Bid adata = ndb_.writeData(aheap);
            const Bid asub = ndb_.writeSubnodes(att_subs.entries());
            subs.add(att_nid, adata, asub);

            const std::size_t r = at.addRow(att_nid);
            at.setString(r, PR_DISPLAY_NAME, name);
            at.setString(r, PR_ATTACH_LONG_FILENAME, name);
            at.setString(r, PR_ATTACH_FILENAME, name);
            at.setInt32(r, PR_ATTACH_METHOD, ATTACH_BY_VALUE);
            at.setInt32(r, PR_ATTACH_SIZE, static_cast<std::uint32_t>(a.data.size()));
            at.setString(r, PR_ATTACH_MIME_TAG, a.content_type);
            at.setInt32(r, PR_ATTACH_NUM, static_cast<std::uint32_t>(i));
        }

        SubnodeAllocator at_subs(ndb_);
        const auto heap = at.serialize(at_subs);
        const Bid data = ndb_.writeData(heap);
        const Bid sub = ndb_.writeSubnodes(at_subs.entries());
        subs.add(kNidAttachmentTable, data, sub);
    }

    const auto heap = pc.serialize(subs);
    writeNodeFromHeap(nid, folder_id, heap, subs);

    // ---- row in the parent folder's contents table ------------------------
    f.contents->beginRow(nid);
    f.contents->setString(PR_SUBJECT, msg.subject);
    f.contents->setString(PR_SENDER_NAME,
                          msg.from.name.empty() ? msg.from.email : msg.from.name);
    f.contents->setString(PR_DISPLAY_TO, joinMailboxes(msg.to));
    f.contents->setString(PR_MESSAGE_CLASS, "IPM.Note");
    f.contents->setTime(PR_MESSAGE_DELIVERY_TIME, unixToFiletime(delivered));
    f.contents->setInt32(PR_MESSAGE_FLAGS, flags);
    f.contents->setInt32(PR_MESSAGE_SIZE, size);
    f.contents->setBool(PR_HASATTACH, !msg.attachments.empty());
    f.contents->endRow();
    ++f.message_count;
    if (!msg.seen()) ++f.unread;
}

// -------------------------------------------------------------- finalisation

void PstWriter::writeMessageStore() {
    SubnodeAllocator subs(ndb_);
    PropertyContext pc;
    pc.setBinary(PR_RECORD_KEY, store_guid_, 16);
    pc.setString(PR_DISPLAY_NAME, "Personal Folders");
    pc.setBinary(PR_IPM_SUBTREE_ENTRYID, entryId(ipm_subtree_));
    pc.setBinary(PR_IPM_WASTEBASKET_ENTRYID, entryId(ipm_subtree_));
    pc.setBinary(PR_IPM_SENTMAIL_ENTRYID, entryId(ipm_subtree_));
    pc.setBinary(PR_IPM_OUTBOX_ENTRYID, entryId(ipm_subtree_));
    pc.setBinary(PR_FINDER_ENTRYID, entryId(root_folder_));
    pc.setBinary(PR_ENTRYID, entryId(root_folder_));
    // Bits 0..5 mark which of the special folder entry ids above are valid.
    pc.setInt32(PR_VALID_FOLDER_MASK, 0x0000003F);

    const auto heap = pc.serialize(subs);
    writeNodeFromHeap(kNidMessageStore, 0, heap, subs);
}

void PstWriter::writeNameIdMap() {
    SubnodeAllocator subs(ndb_);
    PropertyContext pc = names_.buildPropertyContext();
    const auto heap = pc.serialize(subs);
    writeNodeFromHeap(kNidNameToIdMap, 0, heap, subs);
}

void PstWriter::writeFolders() {
    for (auto& f : folders_) {
        // The folder object itself.
        {
            SubnodeAllocator subs(ndb_);
            PropertyContext pc;
            pc.setString(PR_DISPLAY_NAME, f.name);
            pc.setInt32(PR_CONTENT_COUNT, static_cast<std::uint32_t>(f.message_count));
            pc.setInt32(PR_CONTENT_UNREAD, f.unread);
            pc.setBool(PR_SUBFOLDERS, !f.children.empty());
            pc.setString(PR_CONTAINER_CLASS, "IPF.Note");
            pc.setBinary(PR_ENTRYID, entryId(f.nid));
            pc.setBinary(PR_RECORD_KEY, entryId(f.nid));
            const auto heap = pc.serialize(subs);
            writeNodeFromHeap(f.nid, f.parent, heap, subs);
        }

        // Hierarchy table: one row per child folder.
        {
            TableContext ht;
            ht.addColumn(PR_DISPLAY_NAME);
            ht.addColumn(PR_CONTENT_COUNT);
            ht.addColumn(PR_CONTENT_UNREAD);
            ht.addColumn(PR_SUBFOLDERS);
            ht.addColumn(PR_CONTAINER_CLASS);
            for (Nid child_nid : f.children) {
                const Folder& c = folder(child_nid);
                const std::size_t r = ht.addRow(c.nid);
                ht.setString(r, PR_DISPLAY_NAME, c.name);
                ht.setInt32(r, PR_CONTENT_COUNT,
                            static_cast<std::uint32_t>(c.message_count));
                ht.setInt32(r, PR_CONTENT_UNREAD, c.unread);
                ht.setBool(r, PR_SUBFOLDERS, !c.children.empty());
                ht.setString(r, PR_CONTAINER_CLASS, "IPF.Note");
            }
            SubnodeAllocator subs(ndb_);
            const auto heap = ht.serialize(subs);
            writeNodeFromHeap(makeNid(kNidTypeHierarchyTable, nidIndex(f.nid)), f.nid,
                              heap, subs);
        }

        // Contents table.  Its blocks were emitted as messages arrived; this
        // only closes it out and registers the node.
        {
            const auto node = f.contents->finish();
            ndb_.addNode(makeNid(kNidTypeContentsTable, nidIndex(f.nid)), node.data,
                         node.sub, f.nid);
        }

        // Associated contents table: structurally present but always empty.
        {
            TableContext fai;
            fai.addColumn(PR_MESSAGE_CLASS);
            SubnodeAllocator subs(ndb_);
            const auto heap = fai.serialize(subs);
            writeNodeFromHeap(makeNid(kNidTypeAssocContentsTable, nidIndex(f.nid)),
                              f.nid, heap, subs);
        }
    }
}

void PstWriter::finish() {
    if (finished_) return;
    finished_ = true;
    writeFolders();
    writeMessageStore();
    // Last, so that every named property minted while writing messages is in.
    writeNameIdMap();
    ndb_.finish();
}

}  // namespace imap2pst::pst
