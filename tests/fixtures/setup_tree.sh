#!/usr/bin/env sh
# Regenerate scan_tree fixture files with deterministic content.
set -e
ROOT="$(cd "$(dirname "$0")/scan_tree" && pwd)"
mkdir -p "$ROOT/sub"
printf '%s' 'hello world' >"$ROOT/file_a.txt"
printf '%s' 'second file' >"$ROOT/file_b.txt"
printf '%s' 'nested here' >"$ROOT/sub/nested.txt"
echo "scan_tree fixture updated under $ROOT"
