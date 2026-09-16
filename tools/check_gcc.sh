#!/usr/bin/env bash
# Compile every translation unit with GCC as well as Clang.
#
# Only Clang on arm64 had ever built this project, and the first CI run found
# three things GCC objects to that Clang does not -- one of them a real gap in
# an optimisation, one a missing include that libc++ happened to provide
# transitively, and one a false positive worth working around rather than
# suppressing. Waiting for CI to find those is a slow way to learn them.
#
# Needs a GCC; set CXX_GCC to pick one.
set -uo pipefail
cd "$(dirname "$0")/.."
GCC=${CXX_GCC:-g++-16}
if ! command -v "$GCC" >/dev/null 2>&1; then
  echo "no $GCC on this machine; skipping the GCC check"
  exit 0
fi
fail=0
for f in tests/test_*.cpp src/*.cpp apps/*.cpp; do
  out=$("$GCC" -std=c++20 -O3 -DNDEBUG -DITCH_INVARIANT_LEVEL=0 \
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion \
    -Wold-style-cast -Wnon-virtual-dtor -Wformat=2 -Wdouble-promotion \
    -Iinclude -Itests -Ibench -c "$f" -o /tmp/gcc_check.o 2>&1)
  if [ -n "$out" ]; then
    printf "%-28s FAIL\n" "$(basename $f)"
    echo "$out" | grep -E "error:|warning:" | head -3 | sed 's/^/    /'
    fail=1
  else
    printf "%-28s ok\n" "$(basename $f)"
  fi
done
exit $fail
