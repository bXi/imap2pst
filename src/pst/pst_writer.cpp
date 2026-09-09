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

    // A store with no named properties at all leaves the name-to-id map's entry
    // stream empty, and a reader that expects it to exist cannot open the file.
    // Real stores always carry at least this one -- it backs Outlook categories
    // -- so registering it up front keeps the map well formed for a mailbox
    // whose messages happen to have no unrecognised headers.
    names_.idForString(kPsPublicStrings, "Keywords");
    names_.ensureGuid(kPsInternetHeaders);

    // The root node of the folder tree, then the subtree everything the user
    // can see hangs off.
    makeFolder(0, "Root - Mailbox", kNidRootFolder);
    // The root folder is its own parent.  A reader walking up the folder tree
    // uses that as the terminator; a parent of zero sends it to a node that
    // does not exist, and Outlook faults on it before it opens anything.
    folders_.front().parent = kNidRootFolder;
    ipm_subtree_ = makeFolder(kNidRootFolder, "Top of Personal Folders", 0);

    // Every folder the message store advertises has to be real.  Aliasing them
    // onto the subtree, as an earlier version did, both lies to the reader and
    // makes "delete" target the subtree root.
    deleted_items_ = makeFolder(ipm_subtree_, "Deleted Items", 0);
    sent_items_    = makeFolder(ipm_subtree_, "Sent Items", 0);
    outbox_        = makeFolder(ipm_subtree_, "Outbox", 0);
    search_root_   = makeFolder(root_folder_, "Search Root", 0);
    views_         = makeFolder(root_folder_, "Views", 0);
    common_views_  = makeFolder(root_folder_, "Common Views", 0);
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

std::vector<std::uint8_t> PstWriter::oneOffEntryId(const std::string& display_name,
                                                   const std::string& email) {
    // [MS-OXCDATA] 2.2.5.1: flags, the one-off provider UID, version and flags,
    // then display name, address type and address as UTF-16 strings.
    static const std::uint8_t kOneOffUid[16] = {0x81, 0x2B, 0x1F, 0xA4, 0xBE, 0xA3,
                                                0x10, 0x19, 0x9D, 0x6E, 0x00, 0xDD,
                                                0x01, 0x0F, 0x54, 0x02};
    std::vector<std::uint8_t> e;
    put32(e, 0);
    putBytes(e, kOneOffUid, 16);
    put16(e, 0);       // version
    put16(e, 0x1000);  // MAPI_UNICODE
    auto put_string = [&e](const std::string& s) {
        const auto utf16 = utf8ToUtf16le(s);
        e.insert(e.end(), utf16.begin(), utf16.end());
        put16(e, 0);  // terminator
    };
    put_string(display_name);
    put_string("SMTP");
    put_string(email);
    return e;
}

std::vector<PropTag> PstWriter::recipientColumns() {
    return {PR_RECIPIENT_TYPE, PR_RESPONSIBILITY, PR_RECORD_KEY, PR_OBJECT_TYPE,
            PR_ENTRYID,        PR_DISPLAY_NAME,   PR_ADDRTYPE,   PR_EMAIL_ADDRESS,
            PR_SEARCH_KEY,     PR_DISPLAY_TYPE,   PR_7BIT_DISPLAY_NAME,
            PR_SEND_RICH_INFO};
}

std::vector<PropTag> PstWriter::contentsColumns() {
    return {PR_SUBJECT,        PR_SENDER_NAME,    PR_DISPLAY_TO,
            PR_MESSAGE_CLASS,  PR_MESSAGE_DELIVERY_TIME, PR_MESSAGE_FLAGS,
            PR_MESSAGE_SIZE,   PR_HASATTACH,      PR_IMPORTANCE,
            PR_SENSITIVITY,    PR_CLIENT_SUBMIT_TIME, PR_SENT_REPRESENTING_NAME,
            PR_MESSAGE_TO_ME,  PR_MESSAGE_CC_ME,  PR_CONVERSATION_TOPIC,
            PR_CONVERSATION_INDEX, PR_DISPLAY_CC, PR_MESSAGE_STATUS,
            PR_REPL_ITEMID,    PR_REPL_CHANGENUM, PR_REPL_VERSION_HISTORY,
            PR_REPL_FLAGS,     PR_REPL_COPIEDFROM_VERSION,
            PR_REPL_COPIEDFROM_ITEMID, PR_ITEM_TEMPORARY_FLAGS,
            PR_LAST_MODIFICATION_TIME, PR_CONVERSATION_ID,
            PR_SECURE_SUBMIT_FLAGS};
}

std::vector<PropTag> PstWriter::hierarchyColumns() {
    return {PR_DISPLAY_NAME,  PR_CONTENT_COUNT, PR_CONTENT_UNREAD,
            PR_SUBFOLDERS,    PR_CONTAINER_CLASS, PR_REPL_ITEMID,
            PR_REPL_CHANGENUM, PR_REPL_VERSION_HISTORY, PR_REPL_FLAGS,
            PR_PST_HIDDEN_COUNT, PR_PST_HIDDEN_UNREAD};
}

std::vector<PropTag> PstWriter::associatedColumns() {
    return {PR_MESSAGE_CLASS, PR_DISPLAY_NAME, PR_VD_NAME, PR_VD_FLAGS,
            PR_VD_VERSION,    PR_VD_STRINGS,   PR_VIEW_DESCRIPTOR_FLAGS,
            PR_VIEW_DESCRIPTOR_LINKTO, PR_VIEW_DESCRIPTOR_VIEWFLD,
            PR_VIEW_DESCRIPTOR_NAME,   PR_VIEW_DESCRIPTOR_VERSION};
}

FolderId PstWriter::makeFolder(Nid parent, const std::string& name, Nid forced_nid) {
    Folder f;
    f.nid = forced_nid ? forced_nid
                       : makeNid(kNidTypeNormalFolder, next_folder_index_++);
    f.parent = parent;
    f.name = name;
    f.contents = std::make_unique<TableContextWriter>(ndb_, contentsColumns());
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
    // Only written when there is something to write: an empty property costs a
    // heap slot and tells a reader nothing.
    if (!msg.cc.empty()) pc.setString(PR_DISPLAY_CC, joinMailboxes(msg.cc));
    if (!msg.bcc.empty()) pc.setString(PR_DISPLAY_BCC, joinMailboxes(msg.bcc));

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
    // Outlook requires a search key on every message; it only has to be a
    // stable 16-byte value that is unique within the store.
    {
        std::vector<std::uint8_t> key(store_guid_, store_guid_ + 16);
        for (int i = 0; i < 4; ++i) {
            key[12 + i] ^= static_cast<std::uint8_t>(nid >> (8 * i));
        }
        pc.setBinary(PR_SEARCH_KEY, key);
    }

    applyHeaderProperties(pc, msg);

    // ---- recipient table -------------------------------------------------
    {
        TableContext rt;
        for (PropTag tag : recipientColumns()) rt.addColumn(tag);
        // Row ids start at one: a BTH whose first key is zero is rejected
        // outright ("keys overlap, dwkey=0, dwkeyMin=0"), and that takes the
        // whole recipient table with it.
        std::uint32_t row_id = 1;
        auto add = [&](const Mailbox& m, std::uint32_t type) {
            const std::string display = m.name.empty() ? m.email : m.name;
            const std::size_t r = rt.addRow(row_id++);
            rt.setInt32(r, PR_RECIPIENT_TYPE, type);
            rt.setString(r, PR_DISPLAY_NAME, display);
            rt.setString(r, PR_7BIT_DISPLAY_NAME, display);
            rt.setString(r, PR_EMAIL_ADDRESS, m.email);
            rt.setString(r, PR_ADDRTYPE, "SMTP");
            rt.setInt32(r, PR_OBJECT_TYPE, MAPI_MAILUSER);
            rt.setInt32(r, PR_DISPLAY_TYPE, DT_MAILUSER);
            rt.setBool(r, PR_RESPONSIBILITY, false);
            rt.setBool(r, PR_SEND_RICH_INFO, false);
            const auto eid = oneOffEntryId(display, m.email);
            rt.setBinary(r, PR_ENTRYID, eid.data(), eid.size());
            rt.setBinary(r, PR_RECORD_KEY, eid.data(), eid.size());
            // The conventional search key for an SMTP recipient.
            std::string sk = "SMTP:" + m.email;
            std::transform(sk.begin(), sk.end(), sk.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            sk.push_back('\0');
            rt.setBinary(r, PR_SEARCH_KEY, sk.data(), sk.size());
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
        at.addColumn(PR_ATTACH_RENDERING_POS);

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
            // PidTagAttachSize is the size consumed by the whole Attachment
            // object rather than the length of its payload, which on its own is
            // reported as invalid.  Summed here over every property actually
            // written below: the payload, the three name strings each stored
            // separately, the mime tag and content id, and the fixed-width
            // values.  The exact derivation is undocumented, so this is the
            // best-supported reading rather than a certainty.
            std::size_t attach_size = a.data.size();
            attach_size += name.size() * 2 * 3;  // display, short and long name
            attach_size += a.content_type.size() * 2;
            attach_size += a.content_id.size() * 2;
            attach_size += 5 * 4;  // method, number, rendering position,
                                   // object type and the size value itself
            apc.setInt32(PR_ATTACH_SIZE, static_cast<std::uint32_t>(attach_size));
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
            at.setInt32(r, PR_ATTACH_SIZE, static_cast<std::uint32_t>(attach_size));
            at.setString(r, PR_ATTACH_MIME_TAG, a.content_type);
            at.setInt32(r, PR_ATTACH_NUM, static_cast<std::uint32_t>(i));
            at.setInt32(r, PR_ATTACH_RENDERING_POS, 0xFFFFFFFFu);
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

void PstWriter::writeReservedNodes() {
    // The furniture Outlook expects a store to come with.  Every column set
    // below was read out of a PST that Outlook's own Inbox Repair Tool built
    // from one of ours, so these are its values rather than a guess.
    //
    // The tables are prototypes: they carry columns and no rows, and Outlook
    // uses them when it creates objects of the matching kind.
    struct Template {
        Nid nid;
        std::initializer_list<PropTag> columns;
    };
    static const Template kTemplates[] = {
        {makeNid(kNidTypeHierarchyTable, 0x30),
         {0x0E300102, 0x0E330014, 0x0E340102, 0x0E380003, 0x3001001F, 0x36020003,
          0x36030003, 0x360A000B, 0x3613001F, 0x66350003, 0x66360003}},
        {makeNid(kNidTypeContentsTable, 0x30),
         {0x00170003, 0x001A001F, 0x00360003, 0x0037001F, 0x00390040, 0x0042001F,
          0x0057000B, 0x0058000B, 0x0070001F, 0x00710102, 0x0E03001F, 0x0E04001F,
          0x0E060040, 0x0E070003, 0x0E080003, 0x0E170003, 0x0E300102, 0x0E330014,
          0x0E340102, 0x0E380003, 0x0E3C0102, 0x0E3D0102, 0x10970003, 0x30080040,
          0x30130102, 0x65C60003}},
        {makeNid(kNidTypeAssocContentsTable, 0x30),
         {0x001A001F, 0x0E070003, 0x0E170003, 0x3001001F, 0x6800001F, 0x6803000B,
          0x68051003, 0x682F001F, 0x70030003, 0x70040102, 0x70050102, 0x7006001F,
          0x70070003}},
        {makeNid(kNidTypeSearchContentsTable, 0x30),
         {0x00170003, 0x001A001F, 0x00360003, 0x0037001F, 0x0042001F, 0x0057000B,
          0x0058000B, 0x0E03001F, 0x0E04001F, 0x0E05001F, 0x0E060040, 0x0E070003,
          0x0E080003, 0x0E170003, 0x0E2A000B, 0x30080040, 0x67F10003}},
        {makeNid(kNidTypeReceiveFolderTable, 0x31), {0x001A001F, 0x66050003}},
        {makeNid(kNidTypeOutgoingQueueTable, 0x32),
         {0x000F0040, 0x00390040, 0x0E070003, 0x0E100003, 0x0E140003, 0x0E29001F,
          0x67F10003}},
        {makeNid(kNidTypeAttachmentTable, 0x33),
         {0x0E200003, 0x3704001F, 0x37050003, 0x370B0003}},
        {makeNid(kNidTypeRecipientTable, 0x34),
         {0x0C150003, 0x0E0F000B, 0x0FF90102, 0x0FFE0003, 0x0FFF0102, 0x3001001F,
          0x3002001F, 0x3003001F, 0x300B0102, 0x39000003, 0x39FF001F, 0x3A40000B}},
        // Three more table types the format defines but does not name publicly.
        {makeNid(static_cast<NidType>(0x16), 0x35),
         {0x0E330014, 0x0E370102, 0x0E380003}},
        {makeNid(static_cast<NidType>(0x17), 0x36),
         {0x001A001F, 0x0E300102, 0x0E310102, 0x0E330014, 0x0E340102, 0x0E380003,
          0x0E3E0102}},
        {makeNid(static_cast<NidType>(0x18), 0x37), {0x0E330014, 0x30070040}},
    };

    for (const auto& t : kTemplates) {
        TableContext tc;
        for (PropTag tag : t.columns) tc.addColumn(tag);
        SubnodeAllocator subs(ndb_);
        const auto heap = tc.serialize(subs);
        writeNodeFromHeap(t.nid, 0, heap, subs);
    }

    // These exist in the node tree with no data block at all -- the repaired
    // file records them as bidData 0, bidSub 0 -- so they are placeholders the
    // store is expected to declare rather than objects with content.
    for (Nid nid : {kNidSearchManagementQ, Nid{0xE41}, Nid{0xEC1}, Nid{0xF21}}) {
        ndb_.addNode(nid, 0, 0, 0);
    }
}

void PstWriter::writeMessageStore() {
    SubnodeAllocator subs(ndb_);
    PropertyContext pc;
    pc.setBinary(PR_RECORD_KEY, store_guid_, 16);
    // Zero means "no password on this store".  Outlook reports the property as
    // missing rather than defaulting it.
    pc.setInt32(PR_PST_PASSWORD, 0);
    pc.setString(PR_DISPLAY_NAME, "Personal Folders");
    pc.setBinary(PR_IPM_SUBTREE_ENTRYID, entryId(ipm_subtree_));
    pc.setBinary(PR_IPM_WASTEBASKET_ENTRYID, entryId(deleted_items_));
    pc.setBinary(PR_IPM_SENTMAIL_ENTRYID, entryId(sent_items_));
    pc.setBinary(PR_IPM_OUTBOX_ENTRYID, entryId(outbox_));
    pc.setBinary(PR_FINDER_ENTRYID, entryId(search_root_));
    pc.setBinary(PR_VIEWS_ENTRYID, entryId(views_));
    pc.setBinary(PR_COMMON_VIEWS_ENTRYID, entryId(common_views_));
    pc.setBinary(PR_ENTRYID, entryId(root_folder_));
    // Each bit promises that the matching entry id above resolves to a real
    // folder.  The inbox bit (0x02) is deliberately absent: a PST is not a
    // delivery store and no PidTagIpmInboxEntryId is written, and promising an
    // entry id that does not exist is exactly the kind of thing a reader
    // follows straight into a fault.
    pc.setInt32(PR_VALID_FOLDER_MASK, 0x01 | 0x04 | 0x08 | 0x10 | 0x20 | 0x40 | 0x80);

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
            for (PropTag tag : hierarchyColumns()) ht.addColumn(tag);
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
            for (PropTag tag : associatedColumns()) fai.addColumn(tag);
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
    writeReservedNodes();
    writeMessageStore();
    // Last, so that every named property minted while writing messages is in.
    writeNameIdMap();
    ndb_.finish();
}

}  // namespace imap2pst::pst
