#include "pst/nameid_map.h"

#include <cstring>

#include "pst/crc.h"

namespace imap2pst::pst {

const Guid kPsMapi{0x00020328, 0, 0, {0xC0, 0x00, 0, 0, 0, 0, 0, 0x46}};
const Guid kPsPublicStrings{0x00020329, 0, 0, {0xC0, 0x00, 0, 0, 0, 0, 0, 0x46}};
const Guid kPsInternetHeaders{0x00020386, 0, 0, {0xC0, 0x00, 0, 0, 0, 0, 0, 0x46}};

namespace {
// [MS-PST] 2.4.7.1: the bucket count is a fixed prime in practice.
constexpr std::uint32_t kBucketCount = 251;

// MAPI resolves a string-named property without regard to case, so "Content-Type"
// and "Content-type" are one property and must get one id.  Minting two leaves
// Outlook with two map entries for a single name: it reports the store as
// damaged on open, and the Inbox Repair Tool dereferences the entry it could
// not resolve and crashes.  Mail in the wild does vary the spelling of header
// names, so fold case for lookup while keeping the spelling first seen.
std::string foldCase(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}
}  // namespace

std::vector<std::uint8_t> Guid::bytes() const {
    std::vector<std::uint8_t> b;
    put32(b, data1);
    put16(b, data2);
    put16(b, data3);
    putBytes(b, data4, 8);
    return b;
}

bool Guid::operator<(const Guid& o) const {
    return bytes() < o.bytes();
}

std::uint16_t NameIdMap::guidIndex(const Guid& guid) {
    if (!(guid < kPsMapi) && !(kPsMapi < guid)) return 1;
    if (!(guid < kPsPublicStrings) && !(kPsPublicStrings < guid)) return 2;
    for (std::size_t i = 0; i < extra_guids_.size(); ++i) {
        if (!(extra_guids_[i] < guid) && !(guid < extra_guids_[i])) {
            return static_cast<std::uint16_t>(3 + i);
        }
    }
    extra_guids_.push_back(guid);
    return static_cast<std::uint16_t>(2 + extra_guids_.size());
}

std::uint16_t NameIdMap::idForString(const Guid& guid, const std::string& name) {
    const std::uint16_t wguid = guidIndex(guid);
    const auto key = std::make_pair(wguid, foldCase(name));
    auto it = by_string_.find(key);
    if (it != by_string_.end()) return it->second;
    if (entries_.size() >= kMaxNames) return 0;

    const std::vector<std::uint8_t> utf16 = utf8ToUtf16le(name);
    const auto offset = static_cast<std::uint32_t>(string_stream_.size());
    put32(string_stream_, static_cast<std::uint32_t>(utf16.size()));
    string_stream_.insert(string_stream_.end(), utf16.begin(), utf16.end());
    string_stream_.resize(alignUp(string_stream_.size(), 4), 0);

    Entry e;
    e.is_string = true;
    e.numeric = offset;
    e.guid_index = wguid;
    e.prop_index = static_cast<std::uint16_t>(entries_.size());
    e.hash = computeCrc(utf16.data(), utf16.size());
    entries_.push_back(e);

    const auto id = static_cast<std::uint16_t>(kFirstNamedId + e.prop_index);
    by_string_[key] = id;
    return id;
}

std::uint16_t NameIdMap::idForNumeric(const Guid& guid, std::uint32_t id) {
    const std::uint16_t wguid = guidIndex(guid);
    const auto key = std::make_pair(wguid, id);
    auto it = by_numeric_.find(key);
    if (it != by_numeric_.end()) return it->second;
    if (entries_.size() >= kMaxNames) return 0;

    Entry e;
    e.is_string = false;
    e.numeric = id;
    e.guid_index = wguid;
    e.prop_index = static_cast<std::uint16_t>(entries_.size());
    e.hash = id;
    entries_.push_back(e);

    const auto prop_id = static_cast<std::uint16_t>(kFirstNamedId + e.prop_index);
    by_numeric_[key] = prop_id;
    return prop_id;
}

PropertyContext NameIdMap::buildPropertyContext() const {
    auto encode = [](const Entry& e) {
        std::vector<std::uint8_t> b;
        put32(b, e.numeric);
        put16(b, static_cast<std::uint16_t>((e.guid_index << 1) | (e.is_string ? 1u : 0u)));
        put16(b, e.prop_index);
        return b;
    };

    std::vector<std::uint8_t> guid_stream;
    for (const auto& g : extra_guids_) {
        const auto b = g.bytes();
        guid_stream.insert(guid_stream.end(), b.begin(), b.end());
    }

    std::vector<std::uint8_t> entry_stream;
    std::vector<std::vector<std::uint8_t>> buckets(kBucketCount);
    for (const auto& e : entries_) {
        const auto enc = encode(e);
        entry_stream.insert(entry_stream.end(), enc.begin(), enc.end());
        const std::uint32_t bucket = (e.hash ^ e.guid_index) % kBucketCount;
        buckets[bucket].insert(buckets[bucket].end(), enc.begin(), enc.end());
    }

    PropertyContext pc;
    pc.setInt32(PidTagNameidBucketCount, kBucketCount);
    pc.setBinary(PidTagNameidStreamGuid, guid_stream);
    pc.setBinary(PidTagNameidStreamEntry, entry_stream);
    pc.setBinary(PidTagNameidStreamString, string_stream_);
    for (std::uint32_t i = 0; i < kBucketCount; ++i) {
        if (buckets[i].empty()) continue;
        pc.setBinary(makeTag(static_cast<std::uint16_t>(0x1000 + i), kPtBinary),
                     buckets[i]);
    }
    return pc;
}

}  // namespace imap2pst::pst
