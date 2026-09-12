#!/usr/bin/env python3
"""Round-trip verification of a PST written by imap2pst, using libpff as oracle.

The C++ side writes a PST plus a JSON manifest of what it put in.  This script
reads the PST back through pypff -- code that shares nothing with the writer --
and checks the two agree.

Usage:
    verify_pst.py FILE.pst MANIFEST.json

Requires pypff (pip install libpff-python).  This is a test-only dependency;
nothing in the build needs it.
"""

import binascii
import json
import struct
import sys

try:
    import pypff
except ImportError:
    sys.stderr.write(
        "verify_pst.py: pypff is not installed.\n"
        "Install it with:  pip install libpff-python\n"
    )
    sys.exit(77)  # CMake treats 77 as "skipped"

IPM_SUBTREE = "Top of Personal Folders"
PR_MESSAGE_CLASS = 0x001A
PR_RTF_COMPRESSED = 0x1009
RTF_UNCOMPRESSED_MAGIC = 0x414C454D
PR_ATTACH_DATA = 0x3701
PR_ATTACH_METHOD = 0x3705
PT_OBJECT = 0x000D
ATTACH_EMBEDDED_MSG = 5
PID_NAMEID_STREAM_ENTRY = 0x0003
PID_NAMEID_STREAM_STRING = 0x0004


class Failure(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


def find_root_folder(pff_file):
    """The synthetic root item's children are the reserved top-level nodes."""
    root_item = pff_file.get_root_item()
    for i in range(root_item.get_number_of_sub_items()):
        item = root_item.get_sub_item(i)
        if item.identifier == 0x122:
            return item
    raise Failure("no root folder (NID 0x122) in the PST")


def child_named(folder, name):
    for i in range(folder.number_of_sub_folders):
        sub = folder.get_sub_folder(i)
        if sub.get_name() == name:
            return sub
    return None


def resolve(root, path):
    """Walks 'Inbox/Nested Folder' below the IPM subtree."""
    current = child_named(root, IPM_SUBTREE)
    check(current is not None, "IPM subtree %r is missing" % IPM_SUBTREE)
    for part in path.split("/"):
        nxt = child_named(current, part)
        check(nxt is not None,
              "folder %r not found under %r" % (part, current.get_name()))
        current = nxt
    return current


def message_by_subject(folder, subject):
    for i in range(folder.number_of_sub_messages):
        message = folder.get_sub_message(i)
        if message.subject == subject:
            return message
    return None


def as_text(value):
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return value


def strip_nul(text):
    # libpff hands back the stored string; PidTag values are not NUL padded but
    # be tolerant if a reader ever appends one.
    return text.rstrip("\x00")


def verify_message(folder, spec):
    subject = spec["subject"]
    message = message_by_subject(folder, subject)
    check(message is not None,
          "message %r not found in folder %r" % (subject, spec["folder"]))

    check(as_text(message.sender_name) == spec["sender_name"],
          "sender name for %r: %r != %r"
          % (subject, as_text(message.sender_name), spec["sender_name"]))

    body_text = strip_nul(as_text(message.plain_text_body))
    check(body_text == spec["body_text"],
          "plain body for %r: %r != %r" % (subject, body_text, spec["body_text"]))

    if spec["body_html"]:
        body_html = strip_nul(as_text(message.html_body))
        check(body_html == spec["body_html"],
              "html body for %r: %r != %r" % (subject, body_html, spec["body_html"]))

    expected_attachments = spec["attachments"]
    check(message.number_of_attachments == len(expected_attachments),
          "attachment count for %r: %d != %d"
          % (subject, message.number_of_attachments, len(expected_attachments)))

    for index, expected in enumerate(expected_attachments):
        attachment = message.get_attachment(index)
        if "embedded_subject" in expected:
            verify_embedded(attachment, expected, subject)
            continue
        size = attachment.get_size()
        check(size == expected["size"],
              "attachment %d of %r: size %d != %d"
              % (index, subject, size, expected["size"]))
        data = attachment.read_buffer(size)
        check(binascii.hexlify(data).decode() == expected["data_hex"],
              "attachment %d of %r: contents differ" % (index, subject))

    verify_message_class(message, subject, spec.get("message_class"))
    verify_categories(message, subject, spec.get("categories") or [])
    if spec.get("wants_rtf"):
        # Read as a raw property rather than through message.rtf_body: the
        # stream is stored uncompressed, which libpff cannot decode -- it runs
        # its LZ decoder whichever signature it finds.  Outlook accepts only the
        # uncompressed form, so that is what gets written; see rtfFromPlainText.
        value = entries_of(message).get(PR_RTF_COMPRESSED)
        check(value is not None, "message %r should carry an RTF body" % subject)
        data = value[1]
        check(len(data) >= 16, "RTF stream of %r is truncated" % subject)
        magic = struct.unpack("<I", data[8:12])[0]
        check(magic == RTF_UNCOMPRESSED_MAGIC,
              "RTF stream of %r: magic 0x%08X" % (subject, magic))
        check(data[16:].startswith(b"{\\rtf1"),
              "RTF stream of %r does not start an RTF document" % subject)

    return message


def entries_of(item):
    """Every property on an item, as {tag: (value_type, data)}."""
    out = {}
    for i in range(item.number_of_record_sets):
        record_set = item.get_record_set(i)
        for k in range(record_set.number_of_entries):
            entry = record_set.get_entry(k)
            out[entry.entry_type] = (entry.value_type, entry.get_data())
    return out


def verify_message_class(message, subject, expected_class):
    if not expected_class:
        return
    value = entries_of(message).get(PR_MESSAGE_CLASS)
    check(value is not None, "message %r has no message class" % subject)
    text = strip_nul(value[1].decode("utf-16-le", "replace"))
    check(text == expected_class,
          "message class for %r: %r != %r" % (subject, text, expected_class))


def verify_categories(message, subject, expected):
    """Categories are a named property, so find it by value rather than by tag."""
    if not expected:
        return
    found = None
    for tag, (value_type, data) in entries_of(message).items():
        if tag < 0x8000 or value_type != 0x101F:
            continue
        count = struct.unpack("<I", data[0:4])[0]
        offsets = [struct.unpack("<I", data[4 + 4 * i:8 + 4 * i])[0] for i in range(count)]
        bounds = offsets + [len(data)]
        items = [data[bounds[i]:bounds[i + 1]].decode("utf-16-le", "replace")
                 for i in range(count)]
        if items:
            found = items
            break
    check(found == expected,
          "categories for %r: %r != %r" % (subject, found, expected))


def verify_embedded(attachment, expected, subject):
    """Checks what marks an attachment as an embedded message.

    libpff classifies an attachment as an item rather than as data from exactly
    two properties: the method, and the data object being typed as an object
    with the identifier of the subnode holding the message.  Its Python binding
    exposes no way to open that message, so opening it and reading the message
    inside is checked on the binary side instead, by
    PstMessages.EmbeddedMessageIsStoredAsAMessageNotABlob.
    """
    entries = entries_of(attachment)
    method = entries.get(PR_ATTACH_METHOD)
    check(method is not None, "attachment of %r has no method" % subject)
    check(struct.unpack("<I", method[1][:4])[0] == ATTACH_EMBEDDED_MSG,
          "attachment of %r is not marked as an embedded message" % subject)

    data_object = entries.get(PR_ATTACH_DATA)
    check(data_object is not None, "attachment of %r has no data object" % subject)
    value_type, data = data_object
    check(value_type == PT_OBJECT,
          "embedded attachment of %r: value type 0x%04X, expected an object"
          % (subject, value_type))
    check(len(data) >= 8 and struct.unpack("<I", data[:4])[0] != 0,
          "embedded attachment of %r names no subnode" % subject)


def verify_named_properties(pff_file, expected_names):
    """Decodes the name-to-id map streams and checks the expected names are in."""
    name_map = pff_file.get_name_to_id_map()
    record_set = name_map.get_record_set(0)
    streams = {}
    for i in range(record_set.get_number_of_entries()):
        entry = record_set.get_entry(i)
        streams[entry.entry_type] = entry.get_data()

    entry_stream = streams.get(PID_NAMEID_STREAM_ENTRY)
    string_stream = streams.get(PID_NAMEID_STREAM_STRING) or b""
    check(entry_stream is not None, "name-to-id map has no entry stream")

    found = {}
    for offset in range(0, len(entry_stream) - 7, 8):
        value, kind, index = struct.unpack("<IHH", entry_stream[offset:offset + 8])
        is_string = kind & 1
        if not is_string:
            continue
        length = struct.unpack("<I", string_stream[value:value + 4])[0]
        name = string_stream[value + 4:value + 4 + length].decode("utf-16-le")
        found[name] = 0x8000 + index

    for name in expected_names:
        check(name in found, "named property %r is not in the name-to-id map" % name)
    return found


def main(argv):
    # --open-only checks that libpff can open and walk the file at all, which
    # is enough to catch a malformed name-to-id map.
    if len(argv) == 3 and argv[1] == "--open-only":
        pff_file = pypff.file()
        pff_file.open(argv[2])
        try:
            root = find_root_folder(pff_file)
            count = [0]

            def walk(folder):
                count[0] += folder.number_of_sub_messages
                for i in range(folder.number_of_sub_messages):
                    folder.get_sub_message(i).subject
                for i in range(folder.number_of_sub_folders):
                    walk(folder.get_sub_folder(i))

            walk(root)
        except Failure as failure:
            sys.stderr.write("FAIL: %s\n" % failure)
            return 1
        finally:
            pff_file.close()
        print("opened and walked %d message(s) via libpff" % count[0])
        return 0

    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    pst_path, manifest_path = argv[1], argv[2]

    with open(manifest_path, "r", encoding="utf-8") as handle:
        manifest = json.load(handle)

    pff_file = pypff.file()
    pff_file.open(pst_path)
    try:
        root = find_root_folder(pff_file)
        checked = 0
        for spec in manifest["messages"]:
            folder = resolve(root, spec["folder"])
            verify_message(folder, spec)
            checked += 1

        for spec in manifest.get("folder_counts", []):
            folder = resolve(root, spec["folder"])
            check(folder.number_of_sub_messages == spec["count"],
                  "folder %r holds %d message(s), expected %d"
                  % (spec["folder"], folder.number_of_sub_messages, spec["count"]))
            subjects = [folder.get_sub_message(i).subject
                        for i in range(folder.number_of_sub_messages)]
            check(subjects[0] == spec["first_subject"],
                  "first message of %r is %r, expected %r"
                  % (spec["folder"], subjects[0], spec["first_subject"]))
            check(subjects[-1] == spec["last_subject"],
                  "last message of %r is %r, expected %r"
                  % (spec["folder"], subjects[-1], spec["last_subject"]))
            check(len(set(subjects)) == len(subjects),
                  "folder %r has duplicate subjects" % spec["folder"])
            checked += len(subjects)

        named = verify_named_properties(pff_file, manifest["named_properties"])
    except Failure as failure:
        sys.stderr.write("FAIL: %s\n" % failure)
        return 1
    finally:
        pff_file.close()

    print("verified %d message(s) and %d named propert(y|ies) via libpff"
          % (checked, len(named)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
