#!/usr/bin/env bash
# Restore files renamed for Gmail delivery: *.js.txt -> *.js.
# Run this after unzipping the delivery bundle:
#   ./tools/restore_delivery.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

count=0
while IFS= read -r -d '' f; do
    mv "$f" "${f%.js.txt}.js"
    echo "restore: $f -> ${f%.js.txt}.js"
    count=$((count + 1))
done < <(find . -type f -name '*.js.txt' -print0)

echo "restore: renamed $count file(s) back to .js"
