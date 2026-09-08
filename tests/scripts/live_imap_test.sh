#!/bin/sh
# Opt-in integration test against a real IMAP account.
#
# This is NOT part of the default suite: it needs network access and real
# credentials. Enable it with -DIMAP2PST_LIVE_TEST=ON and run:
#
#   IMAP2PST_HOST=imap.example.com \
#   IMAP2PST_USER=alice@example.com \
#   IMAP2PST_PASSWORD=... \
#   ctest -R imap_live_migration --output-on-failure
#
# IMAP2PST_FOLDER limits it to one folder (default: INBOX).
# IMAP2PST_EXTRA_ARGS passes anything else through, e.g. --no-tls --insecure.

set -eu

exe="${1:?usage: live_imap_test.sh PATH_TO_imap2pst [PATH_TO_verify_pst.py] [PYTHON]}"
verify="${2:-}"
python_bin="${3:-python3}"

: "${IMAP2PST_HOST:?set IMAP2PST_HOST}"
: "${IMAP2PST_USER:?set IMAP2PST_USER}"
: "${IMAP2PST_PASSWORD:?set IMAP2PST_PASSWORD}"
folder="${IMAP2PST_FOLDER:-INBOX}"

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT
out="$workdir/live.pst"

echo "Migrating $IMAP2PST_USER@$IMAP2PST_HOST folder '$folder' -> $out"
# shellcheck disable=SC2086
"$exe" \
  --host "$IMAP2PST_HOST" \
  --user "$IMAP2PST_USER" \
  --password-env IMAP2PST_PASSWORD \
  --folder "$folder" \
  --output "$out" \
  --verbose \
  ${IMAP2PST_EXTRA_ARGS:-}

test -s "$out" || { echo "FAIL: no PST was produced"; exit 1; }
echo "Produced $(wc -c < "$out") bytes"

# If pypff is available, prove the result is at least readable end to end.
if [ -n "$verify" ] && "$python_bin" -c "import pypff" 2>/dev/null; then
  "$python_bin" - "$out" <<'PY'
import sys, pypff
f = pypff.file(); f.open(sys.argv[1])
root = next(i for i in (f.get_root_item().get_sub_item(n)
                        for n in range(f.get_root_item().get_number_of_sub_items()))
            if i.identifier == 0x122)
total = 0
def walk(folder):
    global total
    total += folder.number_of_sub_messages
    for i in range(folder.number_of_sub_messages):
        folder.get_sub_message(i).subject
    for i in range(folder.number_of_sub_folders):
        walk(folder.get_sub_folder(i))
walk(root)
f.close()
print("libpff read back %d message(s)" % total)
PY
else
  echo "pypff not available; skipped the libpff read-back check"
fi
