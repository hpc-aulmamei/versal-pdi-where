#!/bin/bash
# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 ARH Laboratory
#  SPDX-License-Identifier: MIT
# ##################################################################################################
#
# Run pdi_where on real partials whose pblock is known, as listed in a manifest. Partials that are
# not present are skipped and counted. Exit 1 if any partial lands in the wrong pblock.
#
#   BUILDS=/path/to/builds tests/test_real.sh [MANIFEST]     (default: tests/slash.manifest)
#
# Manifest lines (blank lines and # comments ignored), whitespace-separated:
#   EXPECTED  XDCS  GLOB...
#   EXPECTED  "<xdc file name>:<pblock>", the pblock the partials must land in
#   XDCS      comma-separated XDC paths, relative to the manifest's directory unless absolute
#   GLOB      partial .pdi or .vbin paths; $VARIABLES and globs are expanded
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${PDI_WHERE:-$HERE/../pdi_where}
MANIFEST=${1:-$HERE/slash.manifest}
MDIR=$(cd "$(dirname "$MANIFEST")" && pwd)
[ -x "$BIN" ] || make -C "$HERE/.." >/dev/null || exit 2

fails=0 total=0 skipped=0
while read -r exp xdcs rest; do
    [[ -z "${exp:-}" || "$exp" == \#* ]] && continue
    args=()
    IFS=, read -ra xs <<< "$xdcs"
    for x in "${xs[@]}"; do [[ "$x" == /* ]] || x="$MDIR/$x"; args+=(--xdc "$x"); done
    files=()
    for g in $rest; do
        for f in $(eval echo "$g"); do
            if [ -f "$f" ]; then files+=("$f"); else skipped=$((skipped + 1)); fi
        done
    done
    for f in "${files[@]}"; do
        total=$((total + 1))
        "$BIN" "${args[@]}" --json "$f" 2>/dev/null | python3 -c '
import json, os, sys
exp, name = sys.argv[1], sys.argv[2]
for r in json.load(sys.stdin):
    ok = [c for c in r["candidates"] if c["pass"]]
    best = min(ok, key=lambda c: (c["rows_outside"], c["share_distance"])) if ok else None
    got = os.path.basename(best["xdc"]) + ":" + best["pblock"] if best else "-"
    good = got == exp
    print("%s %-9s %s %-40s %s" % ("ok  " if good else "FAIL", r["status"],
                                   "comp  " if r["compressed"] else "nocomp", exp, name[-70:])
          + ("" if good else "   got " + got))
    sys.exit(0 if good else 1)
' "$exp" "$f" || fails=$((fails + 1))
    done
done < "$MANIFEST"
echo "$((total - fails)) of $total placed correctly, $skipped path(s) not present"
[ "$fails" -eq 0 ] && [ "$total" -gt 0 ]
