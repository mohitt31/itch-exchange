#!/usr/bin/env bash
# There is no floating point below apps/. ITCH prices are integers with four
# implied decimals and they stay integers through the book, the engine and the
# journal -- a single double would make replay results depend on optimisation
# flags. -Wdouble-promotion is the compiler-side tripwire; this is the source
# side, because a deliberate `double x` compiles without warning.
#
# Line comments are stripped before matching so that prose may mention them.
set -euo pipefail
cd "$(dirname "$0")/.."

dirs=(include/itch/core include/itch/wire include/itch/book include/itch/engine
      include/itch/replay src)
found=0

for d in "${dirs[@]}"; do
  [ -d "$d" ] || continue
  while IFS= read -r -d '' f; do
    if hits=$(sed 's://.*$::' "$f" | grep -nE '(^|[^[:alnum:]_])(float|double|long double)([^[:alnum:]_]|$)'); then
      echo "floating point in $f:"
      echo "$hits" | sed 's/^/  /'
      found=1
    fi
  done < <(find "$d" -type f \( -name '*.hpp' -o -name '*.cpp' \) -print0)
done

if [ "$found" -ne 0 ]; then
  echo "error: floating point is not allowed below apps/" >&2
  exit 1
fi
echo "no floating point below apps/: ok"
