#include "imap/imap_client.h"

#include <curl/curl.h>

#include <cstring>
#include <sstream>

namespace imap2pst::imap {
namespace {

std::size_t appendToString(char* ptr, std::size_t size, std::size_t nmemb, void* user) {
    auto* out = static_cast<std::string*>(user);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// Percent-encodes the characters that would otherwise break the URL path.
std::string urlEscapePath(const std::string& in) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : in) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || std::strchr("-._~!$&'()*+,=:@", c) != nullptr;
        if (safe) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace

CurlTransport::CurlTransport(ImapConfig config) : config_(std::move(config)) {
    if (config_.port == 0) config_.port = config_.use_tls ? 993 : 143;
    curl_ = curl_easy_init();
    if (!curl_) throw ImapError("curl_easy_init failed");
}

CurlTransport::~CurlTransport() {
    if (curl_) curl_easy_cleanup(static_cast<CURL*>(curl_));
}

std::string CurlTransport::url(const std::string& mailbox,
                               const std::string& suffix) const {
    std::ostringstream os;
    os << (config_.use_tls ? "imaps://" : "imap://") << config_.host << ':'
       << config_.port << '/';
    if (!mailbox.empty()) os << urlEscapePath(mailbox);
    os << suffix;
    return os.str();
}

std::string CurlTransport::run(const std::string& u, const std::string& custom_request) {
    auto* curl = static_cast<CURL*>(curl_);
    std::string body;

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, u.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, config_.timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, config_.verbose ? 1L : 0L);

    if (!config_.username.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, config_.username.c_str());
    }
    if (!config_.oauth2_bearer.empty()) {
        curl_easy_setopt(curl, CURLOPT_XOAUTH2_BEARER, config_.oauth2_bearer.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BEARER);
    } else if (!config_.password.empty()) {
        curl_easy_setopt(curl, CURLOPT_PASSWORD, config_.password.c_str());
    }
    if (config_.use_tls) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, config_.verify_peer ? 1L : 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, config_.verify_peer ? 2L : 0L);
    } else {
        // Still upgrade opportunistically when the server offers STARTTLS.
        curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_TRY));
    }
    if (!custom_request.empty()) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, custom_request.c_str());
    }

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        throw ImapError(std::string("IMAP request failed: ") + curl_easy_strerror(rc) +
                        " (" + u + (custom_request.empty() ? "" : " / " + custom_request) + ")");
    }
    return body;
}

std::string CurlTransport::command(const std::string& mailbox,
                                   const std::string& command_line) {
    return run(url(mailbox), command_line);
}

std::string CurlTransport::fetchBody(const std::string& mailbox, std::uint32_t uid) {
    return run(url(mailbox, ";UID=" + std::to_string(uid)), {});
}

// --------------------------------------------------------------- ImapClient

std::vector<FolderInfo> ImapClient::listFolders() {
    const std::string response = transport_->command({}, "LIST \"\" \"*\"");
    return parseListResponse(response);
}

std::vector<MessageMeta> ImapClient::listMessages(const std::string& mailbox) {
    const std::string response =
        transport_->command(mailbox, "UID FETCH 1:* (FLAGS INTERNALDATE RFC822.SIZE)");
    return parseFetchResponse(response);
}

RawMessage ImapClient::fetchMessage(const std::string& mailbox, const MessageMeta& meta) {
    RawMessage raw;
    raw.uid = meta.uid;
    raw.flags = meta.flags;
    raw.keywords = meta.keywords;
    raw.internal_date = meta.internal_date;
    raw.rfc822 = transport_->fetchBody(mailbox, meta.uid);
    return raw;
}

std::vector<RawMessage> ImapClient::fetchFolder(const std::string& mailbox) {
    std::vector<RawMessage> out;
    for (const auto& meta : listMessages(mailbox)) {
        out.push_back(fetchMessage(mailbox, meta));
    }
    return out;
}

}  // namespace imap2pst::imap
