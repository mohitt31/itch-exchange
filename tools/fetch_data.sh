#!/usr/bin/env bash
# Fetches the NASDAQ TotalView-ITCH 5.0 sample feed and records exactly what was
# fetched, so every number measured from it is traceable to a specific input.
#
# The file is a single gzip stream, so a byte prefix of it decompresses cleanly
# up to the cut and then reports end of input. That is why a partial download is
# still a usable corpus: --bytes takes a prefix, and an interrupted full
# download can simply be used as-is.
#
#   tools/fetch_data.sh                 whole file (4.8 GB compressed)
#   tools/fetch_data.sh --bytes 1G      first 1 GB of the gzip stream
#   tools/fetch_data.sh --date 01302020 a different session
set -euo pipefail
cd "$(dirname "$0")/.."

DATE=01302019
BYTES=""
OUT_DIR=data

while [ $# -gt 0 ]; do
  case "$1" in
    --bytes) BYTES="$2"; shift 2 ;;
    --date)  DATE="$2"; shift 2 ;;
    --out)   OUT_DIR="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${DATE}.NASDAQ_ITCH50.gz"
mkdir -p "$OUT_DIR"

to_bytes() {
  case "$1" in
    *G|*g) echo $(( ${1%[Gg]} * 1024 * 1024 * 1024 )) ;;
    *M|*m) echo $(( ${1%[Mm]} * 1024 * 1024 )) ;;
    *)     echo "$1" ;;
  esac
}

echo "checking $URL"
HEAD=$(curl -sI --max-time 30 "$URL")
TOTAL=$(echo "$HEAD" | awk 'tolower($1)=="content-length:"{gsub(/\r/,"",$2); print $2}')
ETAG=$(echo "$HEAD"  | awk 'tolower($1)=="etag:"{gsub(/\r/,"",$2); print $2}')
LASTMOD=$(echo "$HEAD" | sed -n 's/^[Ll]ast-[Mm]odified: *//p' | tr -d '\r')
if [ -z "$TOTAL" ]; then
  echo "error: server did not report a size" >&2
  exit 1
fi
echo "remote size $TOTAL bytes, etag $ETAG"

if [ -n "$BYTES" ]; then
  WANT=$(to_bytes "$BYTES")
  [ "$WANT" -gt "$TOTAL" ] && WANT=$TOTAL
  RANGE="0-$((WANT - 1))"
  OUT="$OUT_DIR/${DATE}.prefix.gz"
else
  WANT=$TOTAL
  RANGE=""
  OUT="$OUT_DIR/${DATE}.NASDAQ_ITCH50.gz"
fi

AVAIL=$(df -k . | tail -1 | awk '{print $4 * 1024}')
if [ "$AVAIL" -lt "$WANT" ]; then
  echo "error: need $WANT bytes, $AVAIL available" >&2
  exit 1
fi

echo "fetching $WANT bytes -> $OUT"
if [ -n "$RANGE" ]; then
  curl -sS --fail -r "$RANGE" -o "$OUT" "$URL"
else
  # -C - resumes, so an interrupted run can be repeated rather than restarted.
  curl -sS --fail -C - -o "$OUT" "$URL"
fi

GOT=$(wc -c < "$OUT" | tr -d ' ')
SHA=$(shasum -a 256 "$OUT" | awk '{print $1}')

cat > "$OUT_DIR/CORPUS.json" <<JSON
{
  "url": "$URL",
  "session_date": "$DATE",
  "remote_total_bytes": $TOTAL,
  "remote_etag": $ETAG,
  "remote_last_modified": "$LASTMOD",
  "byte_range": "${RANGE:-full}",
  "local_path": "$OUT",
  "local_bytes": $GOT,
  "local_sha256": "$SHA",
  "complete": $([ "$GOT" -eq "$TOTAL" ] && echo true || echo false),
  "fetched_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
JSON

echo
echo "wrote $GOT bytes"
echo "sha256 $SHA"
echo "metadata in $OUT_DIR/CORPUS.json"
