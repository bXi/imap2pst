# Internals

How the PST writer is put together, how it is tested, and what was learned
making Outlook accept its output. Useful if you are changing the format code;
unnecessary if you are only running the tool.

## Layout

```
cmake/Dependencies.cmake   pinned FetchContent declarations
src/core/                  the normalized Message every module speaks
src/imap/                  imap_parse.*  response parsers (no I/O)
                           imap_client.* libcurl transport + folder walking
src/mime/                  vmime-backed normalization
src/pst/                   pst_format.h  on-disk constants and helpers
                           crc.*         the [MS-PST] weak CRC-32
                           ndb.*         blocks, allocation maps, NBT/BBT
                           heap.*        heap-on-node, B-tree-on-heap
                           ltp.*         property and table contexts
                           nameid_map.*  named properties, node 0x61
                           prop_tags.h   the MAPI tags this writer emits
                           pst_writer.*  Message -> folders and messages
src/pipeline/              IMAP -> MIME -> PST
tests/                     unit tests, fixtures, round-trip writer and script
```

```sh
cd build && ctest --output-on-failure
```

## MIME parsing

MIME is parsed by this project rather than by a library. The reason is
licensing: the obvious C++ choice, vmime, is GPLv3 with no linking exception, so
linking it would have decided this project's licence for it. The layers are:

```
charset.cpp          bytes -> UTF-8, and recovery when the charset lies
encodings.cpp        base64 and quoted-printable
headers.cpp          field block, RFC 2047, RFC 2231, addresses
entity.cpp           the multipart tree
message_builder.cpp  tree -> the Message the rest of the tool speaks
mime_parser.cpp      the public entry point, unchanged by any of this
```

Each layer is tested on its own in `tests/test_mime_core.cpp`, because
everything above a layer inherits its mistakes and because the inputs that break
them never appear in well-formed mail.

The switch away from vmime was checked by differential testing rather than by
inspection: both parsers ran over the same 147 real messages, comparing
subjects, senders, recipient counts, bodies, attachment counts, names and
payload sizes. Bodies and attachments were identical; the seven differences were
all cases where the new parser is better — `From: "Alice Example"` with no
address, which vmime truncated at the space, and an inline image where vmime
preferred the content id over the filename. The PSTs built from the two came out
the same size with the same node and block counts.

That harness is not in the tree, because keeping it would mean keeping a GPL
build dependency to test with. It is worth rebuilding if the parser is ever
rewritten again: parse a corpus both ways, compare the normalized `Message`, and
treat every difference as a question rather than a verdict.

## Testing

Four groups run, none of which touches the network:

**Unit tests.** IMAP response parsing is driven by recorded fixtures in
`tests/fixtures/*.txt`; MIME normalization by `.eml` fixtures covering plain
text, multipart with attachments, HTML-only, ISO-8859-1, and UTF-8 with RFC 2047
encoded headers.

**Client and pipeline tests.** `ImapTransport` is an interface, so
`test_imap_client.cpp` and `test_pipeline.cpp` replace it with a fake that
replays fixture responses. The pipeline test runs the whole chain — recorded
IMAP traffic through vmime into a PST — and re-reads the result, so folder
hierarchy, message counts and the folder filter are covered without a mailbox.

**PST binary structure.** `tests/test_pst_binary.cpp` writes PSTs and re-reads
them with a small parser written straight from the spec — not shared with the
writer — asserting the header magic and CRCs, page and block trailers and
signatures, B-tree ordering and depth, that no block overlaps a reserved AMap
or PMap page, and that the allocation map marks every block that exists.

**Round-trip against libpff.** `tests/pst_fixture_writer.cpp` writes a PST plus
a JSON manifest of exactly what went in; `tests/scripts/verify_pst.py` reads the
PST back through `pypff` and checks folder names, subjects, bodies, attachment
bytes and the named-property map against that manifest. libpff shares no code
with this project, so it is a genuine oracle rather than a mirror.

### Test-only prerequisite: pypff

The round-trip test needs `pypff`, the Python binding for libpff. It is **not**
a build dependency and nothing links against it — CMake only needs a Python 3
interpreter to invoke the script.

```sh
python3 -m venv .venv
.venv/bin/pip install libpff-python
cmake -S . -B build -DPython3_EXECUTABLE=$PWD/.venv/bin/python
```

`-DPython3_EXECUTABLE` is only needed when the interpreter holding `pypff` is
not the default `python3`.

When `pypff` is missing the script exits `77` and CTest reports the test as
**skipped**, not failed, so the rest of the suite still runs. Distribution
packages (`python3-libpff`) work equally well, as does shelling out to
`pffexport` by hand for a spot check.

### Opt-in live server test

There is one test that does need a real account. It is off by default and never
runs in CI:

```sh
cmake -S . -B build -DIMAP2PST_LIVE_TEST=ON
cmake --build build -j
IMAP2PST_HOST=imap.example.com \
IMAP2PST_USER=alice@example.com \
IMAP2PST_PASSWORD='...' \
IMAP2PST_FOLDER=INBOX \
ctest --test-dir build -R imap_live_migration --output-on-failure
```

It migrates one folder, asserts a non-empty PST came out, and — when `pypff` is
present — reads every message back through libpff.

---

## Scale

The writer has been run to a million messages. Numbers from this machine,
writing synthetic mail with an attachment every 25th message:

| Messages | Time | Peak RSS | File size |
|---------:|-----:|---------:|----------:|
| 25,000   | 1.1s | 9 MB     | 0.24 GB   |
| 100,000  | 5.2s | 24 MB    | 1.02 GB   |
| 1,000,000| 53.7s| 262 MB   | 10.27 GB  |

Throughput stays flat at roughly 18,000 messages a second as the B-trees
deepen -- the million-message file reached NBT depth 4 and BBT depth 5 -- and
memory is proportional to the message count rather than to the mail, at about
250 bytes a message. That is the index of what has been written; message
bodies and attachments are not held past the message they belong to.

The 10 GB file was checked structurally: `ibFileEof` matches the file, every
B-tree page passes its CRC, and blocks sit at offsets well past 4 GB, so the
64-bit paths are exercised rather than assumed. That check is streamed, because
libpff cannot be used at this size: its open is superlinear in the node count --
2.5s at 50,000 messages, 17.5s at 100,000, and it had not finished a million
after fifteen minutes -- so the round-trip oracle covers correctness on small
files and the probe covers size on large ones.

Outlook opens a 50,000-message, 520 MB store written by this tool.

These measure the writer alone. A real migration is bounded by the IMAP server,
not by this; `--spool` exists so that a second run is not bounded by it twice.

`tests/pst_scale_probe` is the harness. A small run is part of the test suite,
where it guards against throughput decay and per-message memory growth; the
large runs are manual:

```sh
build/tests/pst_scale_probe /tmp/big.pst 1000000 200
```

## What it took to satisfy Outlook

That defect is also a lesson in how it presents. Whether a page happened to land
aligned depended on how many blocks preceded it, so the failure tracked folder
count and placement while following no rule that made sense: one folder under an
empty parent worked, two did not; a subtree with three children worked, two or
four did not. Hours went into hypotheses about child counts and hierarchy
tables. The answer came from attaching a debugger, breaking on the exception,
and reading the format string beside the error code -- worth doing early once a
failure proves deterministic.

Two further cautions. The repair tool's complaints are advisory -- it repairs the
file regardless -- and acting on them twice broke a configuration that
previously opened. And a PST that Outlook has opened is no longer the file that
was written, because it adds its own search folders on first open; diffing one of
those measures the wrong thing.

Not written, and not required for Outlook to open a store cleanly: the search
folders and their update queues, and node `0xEE1`, a flat list with one record
per object that Outlook builds for itself. Writing `0xEE1` in Outlook's exact
format stops even a bare store from opening, so its payload carries meaning this
writer does not understand -- see the comment in
`PstWriter::writeReservedNodes`.

## References

* [MS-PST], *Outlook Personal Folders (.pst) File Format* — the primary
  reference for everything under `src/pst/`.
* [java-libpst](https://github.com/rjohnsondev/java-libpst) — used as a second
  opinion where the spec is ambiguous, particularly around the NDB allocation
  maps and the heap-on-node and property-context layers.
* [libpff](https://github.com/libyal/libpff) — read-only, used as the test
  oracle.

---

Back to the [README](../README.md).

[MS-PST]: https://learn.microsoft.com/en-us/openspecs/office_file_formats/ms-pst/
