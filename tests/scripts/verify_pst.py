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
        size = attachment.get_size()
        check(size == expected["size"],
              "attachment %d of %r: size %d != %d"
              % (index, subject, size, expected["size"]))
        data = attachment.read_buffer(size)
        check(binascii.hexlify(data).decode() == expected["data_hex"],
              "attachment %d of %r: contents differ" % (index, subject))

    return message


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
