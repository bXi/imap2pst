#pragma once

// The name-to-id map, node NID_NAME_TO_ID_MAP (0x61).
//
// Property ids at or above 0x8000 are "named": their real identity is a
// (GUID, name) pair and the number is assigned per file.  This class mints
// those numbers and serializes the three streams -- GUID, entry and string --
// plus the hash buckets that [MS-PST] 2.4.7.5 describes, as a property context.
//
// Note on the buckets: the readers this project verifies against (libpff and
// java-libpst) resolve named properties from the entry stream and ignore the
// buckets entirely, so a bucket hash that disagrees with Outlook's would not be
// caught by the test suite.  The formula used here is hash XOR wGuid, modulo
// the bucket count.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "pst/ltp.h"
#include "pst/pst_format.h"

namespace imap2pst::pst {

struct Guid {
    std::uint32_t data1 = 0;
    std::uint16_t data2 = 0;
    std::uint16_t data3 = 0;
    std::uint8_t data4[8] = {};

    std::vector<std::uint8_t> bytes() const;
    bool operator<(const Guid& o) const;
};

// {00020328-0000-0000-C000-000000000046}
extern const Guid kPsMapi;
// {00020329-0000-0000-C000-000000000046}
extern const Guid kPsPublicStrings;
// {00020386-0000-0000-C000-000000000046}
extern const Guid kPsInternetHeaders;

class NameIdMap {
 public:
    // Named property ids run from 0x8000; stop minting well before the 0xFFFE
    // ceiling so a pathological mailbox cannot exhaust the space.
    static constexpr std::uint16_t kFirstNamedId = 0x8000;
    static constexpr std::size_t kMaxNames = 4096;

    // Returns the property id for a string-named property, minting one on first
    // use.  Returns 0 when the map is full.
    std::uint16_t idForString(const Guid& guid, const std::string& name);
    // Same, for a numerically named property.
    std::uint16_t idForNumeric(const Guid& guid, std::uint32_t id);

    std::size_t size() const { return entries_.size(); }

    // Puts `guid` in the GUID stream without giving it a property.  A reader
    // may require that stream to be non-empty, and the two predefined GUIDs
    // (PS_MAPI and PS_PUBLIC_STRINGS) are referenced by index and contribute no
    // bytes to it, so a store whose only named properties use those would
    // otherwise leave it empty.
    void ensureGuid(const Guid& guid) { guidIndex(guid); }

    // Builds the PC that node 0x61 stores.
    PropertyContext buildPropertyContext() const;

 private:
    struct Entry {
        bool is_string = false;
        std::uint32_t numeric = 0;      // numeric id, or string-stream offset
        std::uint16_t guid_index = 0;   // wGuid as stored on disk
        std::uint16_t prop_index = 0;   // property id - 0x8000
        std::uint32_t hash = 0;
    };

    std::uint16_t guidIndex(const Guid& guid);

    std::vector<Entry> entries_;
    std::vector<Guid> extra_guids_;                     // wGuid 3, 4, 5, ...
    std::vector<std::uint8_t> string_stream_;
    std::map<std::pair<std::uint16_t, std::string>, std::uint16_t> by_string_;
    std::map<std::pair<std::uint16_t, std::uint32_t>, std::uint16_t> by_numeric_;
};

}  // namespace imap2pst::pst
