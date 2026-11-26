#!/usr/bin/env bash
# Tests the page password's gate and trial (components/ice9_lock) on this computer. No board.
#
#   tools/lock-host-test.sh        (a second)
#
# Copies the functions out of components/ice9_lock/page_lock.cpp word for word and runs them in
# tests/lock_flash_test.cpp against a pretend clock, flash and log.
set -uo pipefail
cd "$(dirname "$0")/.."
OUT=$(mktemp -d)
python3 - "$OUT/lock_extracted.inc" <<'PY' || exit 1
import re, sys
src = open("components/ice9_lock/page_lock.cpp").read()
def block(signature):
    start = src.index(signature)
    at = src.index("{", start)
    depth = 0
    while True:
        if src[at] == "{":
            depth += 1
        elif src[at] == "}":
            depth -= 1
            if depth == 0:
                return src[start:at + 1] + "\n\n"
        at += 1
consts = "\n".join(l for l in src.splitlines() if re.match(r"static const .*\b(MAGIC|MIN_LEN)\b", l)) + "\n\n"
parts = ["static bool printable(", "static bool same(", "bool PageLock::admits(", "void PageLock::state(",
         "bool PageLock::change(", "void PageLock::loop("]
open(sys.argv[1], "w").write(consts + "".join(block(p) for p in parts))
PY
c++ -std=c++17 -Wall -Wextra -I"$OUT" -o "$OUT/lock" tests/lock_flash_test.cpp || exit 1
"$OUT/lock"
