#pragma once

// The MAPI property tags this writer emits.
//
// A tag is (property id << 16) | property type.  Ids below 0x8000 are fixed by
// the spec and mean the same thing in every store; ids at or above 0x8000 are
// named properties whose numbering is per-file and lives in the name-to-id map
// (see nameid_map.h).  Everything in this header is in the fixed range.

#include "pst/pst_format.h"

namespace imap2pst::pst {

using PropTag = std::uint32_t;

constexpr PropTag makeTag(std::uint16_t id, PropType type) {
    return (static_cast<std::uint32_t>(id) << 16) | type;
}
constexpr std::uint16_t tagId(PropTag t) { return static_cast<std::uint16_t>(t >> 16); }
constexpr PropType tagType(PropTag t) { return static_cast<PropType>(t & 0xFFFF); }

// ------------------------------------------------------------------ common
inline constexpr PropTag PR_MESSAGE_CLASS          = makeTag(0x001A, kPtUnicode);
inline constexpr PropTag PR_SUBJECT                = makeTag(0x0037, kPtUnicode);
inline constexpr PropTag PR_CLIENT_SUBMIT_TIME     = makeTag(0x0039, kPtSystime);
inline constexpr PropTag PR_SENT_REPRESENTING_NAME = makeTag(0x0042, kPtUnicode);
inline constexpr PropTag PR_CONVERSATION_TOPIC     = makeTag(0x0070, kPtUnicode);
inline constexpr PropTag PR_TRANSPORT_MESSAGE_HEADERS = makeTag(0x007D, kPtUnicode);
inline constexpr PropTag PR_DISPLAY_BCC            = makeTag(0x0E02, kPtUnicode);
inline constexpr PropTag PR_DISPLAY_CC             = makeTag(0x0E03, kPtUnicode);
inline constexpr PropTag PR_DISPLAY_TO             = makeTag(0x0E04, kPtUnicode);
inline constexpr PropTag PR_MESSAGE_DELIVERY_TIME  = makeTag(0x0E06, kPtSystime);
inline constexpr PropTag PR_MESSAGE_FLAGS          = makeTag(0x0E07, kPtLong);
inline constexpr PropTag PR_MESSAGE_SIZE           = makeTag(0x0E08, kPtLong);
inline constexpr PropTag PR_HASATTACH              = makeTag(0x0E1B, kPtBoolean);
inline constexpr PropTag PR_ATTACH_SIZE            = makeTag(0x0E20, kPtLong);
inline constexpr PropTag PR_ATTACH_NUM             = makeTag(0x0E21, kPtLong);
inline constexpr PropTag PR_INTERNET_ARTICLE_NUMBER= makeTag(0x0E23, kPtLong);
inline constexpr PropTag PR_BODY                   = makeTag(0x1000, kPtUnicode);
inline constexpr PropTag PR_HTML                   = makeTag(0x1013, kPtBinary);
inline constexpr PropTag PR_RTF_COMPRESSED         = makeTag(0x1009, kPtBinary);
inline constexpr PropTag PR_RTF_IN_SYNC            = makeTag(0x0E1F, kPtBoolean);
inline constexpr PropTag PR_LAST_VERB_EXECUTED     = makeTag(0x1081, kPtLong);
inline constexpr PropTag PR_LAST_VERB_EXECUTION_TIME = makeTag(0x1082, kPtSystime);
inline constexpr PropTag PR_ICON_INDEX             = makeTag(0x1080, kPtLong);
inline constexpr PropTag PR_INTERNET_MESSAGE_ID    = makeTag(0x1035, kPtUnicode);
inline constexpr PropTag PR_IN_REPLY_TO_ID         = makeTag(0x1042, kPtUnicode);
inline constexpr PropTag PR_FLAG_STATUS            = makeTag(0x1090, kPtLong);
inline constexpr PropTag PR_DISPLAY_NAME           = makeTag(0x3001, kPtUnicode);
inline constexpr PropTag PR_ADDRTYPE               = makeTag(0x3002, kPtUnicode);
inline constexpr PropTag PR_EMAIL_ADDRESS          = makeTag(0x3003, kPtUnicode);
inline constexpr PropTag PR_CREATION_TIME          = makeTag(0x3007, kPtSystime);
inline constexpr PropTag PR_LAST_MODIFICATION_TIME = makeTag(0x3008, kPtSystime);
inline constexpr PropTag PR_OBJECT_TYPE            = makeTag(0x0FFE, kPtLong);
inline constexpr PropTag PR_RECORD_KEY             = makeTag(0x0FF9, kPtBinary);
inline constexpr PropTag PR_ENTRYID                = makeTag(0x0FFF, kPtBinary);
inline constexpr PropTag PR_INTERNET_CPID          = makeTag(0x3FDE, kPtLong);
inline constexpr PropTag PR_MESSAGE_CODEPAGE       = makeTag(0x3FFD, kPtLong);

// ------------------------------------------------------------------ sender
inline constexpr PropTag PR_SENDER_NAME            = makeTag(0x0C1A, kPtUnicode);
inline constexpr PropTag PR_SENDER_ADDRTYPE        = makeTag(0x0C1E, kPtUnicode);
inline constexpr PropTag PR_SENDER_EMAIL_ADDRESS   = makeTag(0x0C1F, kPtUnicode);
inline constexpr PropTag PR_RECIPIENT_TYPE         = makeTag(0x0C15, kPtLong);

// ----------------------------------------------------------------- folders
inline constexpr PropTag PR_CONTENT_COUNT          = makeTag(0x3602, kPtLong);
inline constexpr PropTag PR_CONTENT_UNREAD         = makeTag(0x3603, kPtLong);
inline constexpr PropTag PR_SUBFOLDERS             = makeTag(0x360A, kPtBoolean);
inline constexpr PropTag PR_CONTAINER_CLASS        = makeTag(0x3613, kPtUnicode);

// ----------------------------------------------------------- message store
inline constexpr PropTag PR_VALID_FOLDER_MASK      = makeTag(0x35DF, kPtLong);
inline constexpr PropTag PR_IPM_SUBTREE_ENTRYID    = makeTag(0x35E0, kPtBinary);
inline constexpr PropTag PR_IPM_OUTBOX_ENTRYID     = makeTag(0x35E2, kPtBinary);
inline constexpr PropTag PR_IPM_WASTEBASKET_ENTRYID= makeTag(0x35E3, kPtBinary);
inline constexpr PropTag PR_IPM_SENTMAIL_ENTRYID   = makeTag(0x35E4, kPtBinary);
inline constexpr PropTag PR_VIEWS_ENTRYID          = makeTag(0x35E5, kPtBinary);
inline constexpr PropTag PR_COMMON_VIEWS_ENTRYID   = makeTag(0x35E6, kPtBinary);
inline constexpr PropTag PR_FINDER_ENTRYID         = makeTag(0x35E7, kPtBinary);
inline constexpr PropTag PR_STORE_RECORD_KEY       = makeTag(0x0FF9, kPtBinary);
inline constexpr PropTag PR_PST_PASSWORD           = makeTag(0x67FF, kPtLong);

// ------------------------------------------------------------- attachments
inline constexpr PropTag PR_ATTACH_DATA_BIN        = makeTag(0x3701, kPtBinary);
// The same property id as PR_ATTACH_DATA_BIN, typed as an object: that is how
// an embedded message is distinguished from a blob attachment.
inline constexpr PropTag PR_ATTACH_DATA_OBJ        = makeTag(0x3701, kPtObject);
inline constexpr PropTag PR_ATTACH_ENCODING        = makeTag(0x3702, kPtBinary);
inline constexpr PropTag PR_ATTACH_EXTENSION       = makeTag(0x3703, kPtUnicode);
inline constexpr PropTag PR_ATTACH_FILENAME        = makeTag(0x3704, kPtUnicode);
inline constexpr PropTag PR_ATTACH_METHOD          = makeTag(0x3705, kPtLong);
inline constexpr PropTag PR_ATTACH_LONG_FILENAME   = makeTag(0x3707, kPtUnicode);
inline constexpr PropTag PR_ATTACH_RENDERING_POS   = makeTag(0x370B, kPtLong);
inline constexpr PropTag PR_ATTACH_MIME_TAG        = makeTag(0x370E, kPtUnicode);
inline constexpr PropTag PR_ATTACH_CONTENT_ID      = makeTag(0x3712, kPtUnicode);
inline constexpr PropTag PR_ATTACH_FLAGS           = makeTag(0x3714, kPtLong);

// --------------------------------------------- columns Outlook requires
// Outlook validates that a folder's tables declare a fixed set of columns and
// reports every absent one, even when no row would carry a value.  Declaring a
// column costs a few bytes of row width; the cell-existence bitmap still marks
// it absent per row.
inline constexpr PropTag PR_IMPORTANCE              = makeTag(0x0017, kPtLong);
inline constexpr PropTag PR_SENSITIVITY             = makeTag(0x0036, kPtLong);
inline constexpr PropTag PR_MESSAGE_TO_ME           = makeTag(0x0057, kPtBoolean);
inline constexpr PropTag PR_MESSAGE_CC_ME           = makeTag(0x0058, kPtBoolean);
inline constexpr PropTag PR_CONVERSATION_INDEX      = makeTag(0x0071, kPtBinary);
inline constexpr PropTag PR_MESSAGE_STATUS          = makeTag(0x0E17, kPtLong);
inline constexpr PropTag PR_REPL_ITEMID             = makeTag(0x0E30, kPtBinary);
inline constexpr PropTag PR_REPL_CHANGENUM          = makeTag(0x0E33, kPtLongLong);
inline constexpr PropTag PR_REPL_VERSION_HISTORY    = makeTag(0x0E34, kPtBinary);
inline constexpr PropTag PR_REPL_FLAGS              = makeTag(0x0E38, kPtLong);
inline constexpr PropTag PR_REPL_COPIEDFROM_VERSION = makeTag(0x0E3C, kPtBinary);
inline constexpr PropTag PR_REPL_COPIEDFROM_ITEMID  = makeTag(0x0E3D, kPtBinary);
inline constexpr PropTag PR_ITEM_TEMPORARY_FLAGS    = makeTag(0x1097, kPtLong);
inline constexpr PropTag PR_CONVERSATION_ID         = makeTag(0x3013, kPtBinary);
inline constexpr PropTag PR_SECURE_SUBMIT_FLAGS     = makeTag(0x65C6, kPtLong);
inline constexpr PropTag PR_PST_HIDDEN_COUNT        = makeTag(0x6635, kPtLong);
inline constexpr PropTag PR_PST_HIDDEN_UNREAD       = makeTag(0x6636, kPtLong);
// The folder a receive-folder-table row points at.
inline constexpr PropTag PR_PST_RECEIVE_FOLDER      = makeTag(0x6605, kPtLong);
inline constexpr PropTag PR_SEARCH_KEY              = makeTag(0x300B, kPtBinary);
// Columns Outlook requires a recipient table to declare.
inline constexpr PropTag PR_RESPONSIBILITY          = makeTag(0x0E0F, kPtBoolean);
inline constexpr PropTag PR_DISPLAY_TYPE            = makeTag(0x3900, kPtLong);
inline constexpr PropTag PR_7BIT_DISPLAY_NAME       = makeTag(0x39FF, kPtUnicode);
inline constexpr PropTag PR_SEND_RICH_INFO          = makeTag(0x3A40, kPtBoolean);
// Associated-contents (FAI) tables carry the view-descriptor columns.
inline constexpr PropTag PR_VD_NAME                 = makeTag(0x6800, kPtUnicode);
inline constexpr PropTag PR_VD_FLAGS                = makeTag(0x6803, kPtBoolean);
inline constexpr PropTag PR_VD_VERSION              = makeTag(0x6805, kPtMvLong);
inline constexpr PropTag PR_VD_STRINGS              = makeTag(0x682F, kPtUnicode);
inline constexpr PropTag PR_VIEW_DESCRIPTOR_FLAGS   = makeTag(0x7003, kPtLong);
inline constexpr PropTag PR_VIEW_DESCRIPTOR_LINKTO  = makeTag(0x7004, kPtBinary);
inline constexpr PropTag PR_VIEW_DESCRIPTOR_VIEWFLD = makeTag(0x7005, kPtBinary);
inline constexpr PropTag PR_VIEW_DESCRIPTOR_NAME    = makeTag(0x7006, kPtUnicode);
inline constexpr PropTag PR_VIEW_DESCRIPTOR_VERSION = makeTag(0x7007, kPtLong);

// ------------------------------------------------------- LTP internal props
// Every table context carries these two as its first two columns.
inline constexpr PropTag PidTagLtpRowId            = makeTag(0x67F2, kPtLong);
inline constexpr PropTag PidTagLtpRowVer           = makeTag(0x67F3, kPtLong);

// Name-to-id map streams, [MS-PST] 2.4.7.
inline constexpr PropTag PidTagNameidBucketCount   = makeTag(0x0001, kPtLong);
inline constexpr PropTag PidTagNameidStreamGuid    = makeTag(0x0002, kPtBinary);
inline constexpr PropTag PidTagNameidStreamEntry   = makeTag(0x0003, kPtBinary);
inline constexpr PropTag PidTagNameidStreamString  = makeTag(0x0004, kPtBinary);

// PR_MESSAGE_FLAGS bits we set.
inline constexpr std::uint32_t MSGFLAG_READ        = 0x00000001;
inline constexpr std::uint32_t MSGFLAG_UNSENT      = 0x00000008;
inline constexpr std::uint32_t MSGFLAG_UNMODIFIED  = 0x00000002;
inline constexpr std::uint32_t MSGFLAG_HASATTACH   = 0x00000010;
inline constexpr std::uint32_t MSGFLAG_FROMME      = 0x00000020;

// PR_FLAG_STATUS values.
inline constexpr std::uint32_t FLAG_STATUS_NONE      = 0;
inline constexpr std::uint32_t FLAG_STATUS_COMPLETE  = 1;
inline constexpr std::uint32_t FLAG_STATUS_FLAGGED   = 2;

// PR_ATTACH_METHOD.
// PidTagLastVerbExecuted: the reply arrow Outlook shows on an answered message.
inline constexpr std::uint32_t VERB_REPLYTOSENDER = 102;
inline constexpr std::uint32_t ICON_MAIL_REPLIED  = 0x105;

inline constexpr std::uint32_t ATTACH_BY_VALUE = 1;
inline constexpr std::uint32_t ATTACH_EMBEDDED_MSG = 5;

// PR_RECIPIENT_TYPE.
inline constexpr std::uint32_t MAPI_TO  = 1;
inline constexpr std::uint32_t MAPI_CC  = 2;
inline constexpr std::uint32_t MAPI_BCC = 3;

// PR_OBJECT_TYPE.
inline constexpr std::uint32_t MAPI_MAILUSER  = 6;

// PR_DISPLAY_TYPE.
inline constexpr std::uint32_t DT_MAILUSER = 0;
inline constexpr std::uint32_t MAPI_ATTACH    = 7;

// PR_INTERNET_CPID: everything we write is transcoded to UTF-8.
inline constexpr std::uint32_t CP_UTF8 = 65001;

}  // namespace imap2pst::pst
