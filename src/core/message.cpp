#include "core/message.h"

namespace imap2pst {

std::string Message::header_blob() const {
    std::string out;
    for (const auto& h : headers) {
        out += h.name;
        out += ": ";
        out += h.value;
        out += "\r\n";
    }
    return out;
}

std::vector<std::string> FolderInfo::path() const {
    std::vector<std::string> parts;
    if (delimiter == '\0') {
        if (!full_name.empty()) parts.push_back(full_name);
        return parts;
    }
    std::string cur;
    for (char c : full_name) {
        if (c == delimiter) {
            // Collapse empty components so "INBOX//Work" and a trailing
            // delimiter do not create nameless folders.
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

std::string FolderInfo::leaf_name() const {
    auto parts = path();
    return parts.empty() ? std::string() : parts.back();
}

}  // namespace imap2pst
