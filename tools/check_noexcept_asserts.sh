#!/usr/bin/env bash
# A function that is marked noexcept and contains an assertion cannot be tested
# with ITCH_REQUIRE_ASSERT: the test build's assert handler throws, and throwing
# out of a noexcept function calls std::terminate. That turns "this misuse is
# detected" from a passing test into a crashed test binary.
#
# So the rule is: if it asserts, it is not noexcept. Nowhere in this codebase
# does the annotation buy anything on such a function.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tools/check_noexcept_asserts.py
