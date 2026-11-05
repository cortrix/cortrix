#!/usr/bin/env bash
# Builds the site for publication and fails on any Hugo warning or error but
# one: Hugo 0.168 reports that LibSass is deprecated, and the theme compiles
# its styles with the LibSass built into Hugo Extended. That notice is printed
# and ignored; every other WARN or ERROR line fails the build.
#
# Run from anywhere; extra arguments are passed to hugo.
set -euo pipefail

cd "$(dirname "$0")/.."

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

hugo --cleanDestinationDir --gc --minify --environment production \
  --printPathWarnings "$@" 2>&1 | tee "$log"

allowed='deprecated: css.Sass: libsass was deprecated'
if grep -E '^(WARN|ERROR)' "$log" | grep -vF "$allowed"; then
  echo "build_strict.sh: the build reported the warnings or errors above" >&2
  exit 1
fi
