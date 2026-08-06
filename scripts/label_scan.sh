#!/bin/sh
# Scan the tree for internal tracking labels that must not ship publicly.
#
# Three earlier passes each reported "clean" and each was wrong, because the
# pattern in use required a non-word character before the label. That hid
# every label sitting behind an underscore (collect_p12, test_..._mem05.cpp)
# or inside a CamelCase identifier (ShouldSkipF37 — a public method). This
# script has no word-boundary guard on the left, which is the whole point.
#
# It also checks the two things a rename breaks that no compiler reports:
# doubled words (agent_trace_trace, crag-crag) and stray duplicate copies
# ("foo 2.h") that nothing includes and no test therefore exercises.
#
# Usage: scripts/label_scan.sh   (run from the repo root; exit 0 = clean)
set -e

LABEL='(F[0-9]{2}[a-zA-Z]?|MEM[0-9]{2}|P(0[0-9]|1[0-9])|TD-F42-BULK|f[0-9]{2}[a-z]?|mem[0-9]{2}|p(0[0-9]|1[0-9]))'

# Hex digests and trace ids, float widths, Unicode code points, the brand
# colour and flake8 codes all collide with the pattern and are not labels.
NOISE='f32|f64|sha256|\b[0-9a-fA-F]{8,}\b|0x[0-9a-fA-F]+|#[0-9A-Fa-f]{6}|U\+?[0-9A-F]{4,}|F401|FF6405|ff6405|RingOf|_mm256|package-lock|0B0F19|parent_id:"P01"'

# This script quotes the labels it hunts for, so it must exclude itself.
# The tree list was once a hand-picked subset; sdk/, docs/ and the web root
# sat outside it and quietly kept three dozen labels. Scan everything and
# exclude, never include.
TREES='. :!scripts/label_scan.sh :!tests/fixtures :!web/package-lock.json'

fail=0

echo "== tracked filenames =="
if git ls-files | grep -inE "(^|[^A-Za-z0-9])$LABEL([^A-Za-z0-9]|\$)"; then fail=1; else echo "  clean"; fi

echo "== file contents (no left word-boundary guard) =="
if git grep -nE "$LABEL([^A-Za-z0-9]|\$)" -- $TREES 2>/dev/null | grep -viE "$NOISE"; then fail=1; else echo "  clean"; fi

echo "== labels inside CamelCase identifiers =="
if git grep -nE "[A-Za-z0-9_]$LABEL([^A-Za-z0-9]|\$)" -- $TREES 2>/dev/null | grep -viE "$NOISE"; then fail=1; else echo "  clean"; fi

echo "== suffixed design-doc labels (P02a, F16b ...) =="
if git grep -nE "[PF][0-9]{2}[a-z] " -- $TREES 2>/dev/null | grep -viE "$NOISE"; then fail=1; else echo "  clean"; fi

echo "== development-phase labels and section marks =="
if git grep -nE 'D3\.5|§' -- $TREES 2>/dev/null; then fail=1; else echo "  clean"; fi

echo "== doubled words in quoted strings =="
if git grep -nohE '"[a-z]+([-_][a-z]+)+"' -- src include tests | sort -u | grep -v 'idx_[a-z_]*_id' | python3 -c '
import sys, re
bad = 0
for line in sys.stdin:
    parts = re.split(r"[-_]", line.strip().strip(chr(34)))
    if any(parts[i] == parts[i + 1] for i in range(len(parts) - 1)):
        print("  " + line.strip()); bad = 1
sys.exit(bad)
'; then echo "  clean"; else fail=1; fi

echo "== duplicate copies =="
if git ls-files | grep " 2\."; then fail=1; else echo "  clean"; fi

exit $fail
