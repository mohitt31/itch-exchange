#!/usr/bin/env bash
# Every header must compile on its own. Without this a header can depend on an
# include that only happens to arrive first in some other translation unit, and
# the breakage shows up later in an unrelated file.
set -euo pipefail
cd "$(dirname "$0")/.."

CXX=${CXX:-c++}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
while IFS= read -r -d '' h; do
  rel=${h#include/}
  printf '#include "%s"\n' "$rel" > "$tmp/tu.cpp"
  if ! $CXX -std=c++20 -Iinclude -fsyntax-only "$tmp/tu.cpp" 2>"$tmp/err"; then
    echo "not self-contained: $rel"
    sed 's/^/  /' "$tmp/err" | head -5
    fail=1
  fi
done < <(find include -name '*.hpp' -print0)

if [ "$fail" -ne 0 ]; then
  echo "error: some headers are not self-contained" >&2
  exit 1
fi
echo "all headers are self-contained: ok"
