#!/usr/bin/env bash
# Package the delivery bundle: copy all git-tracked files (source, README,
# sample config, tests, examples, docs, build files) into a staging folder,
# verify no secrets slipped in, then zip it into dist/.
#
# Git-tracked selection inherently excludes gitignored secrets (*.secret,
# .env, config/gateway.json, data/, build/, .opencode/).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

STAGE_DIR="$ROOT/dist/rest_exchange_gateway"
ARCHIVE="$ROOT/dist/rest_exchange_gateway_$(date +%Y%m%d).zip"

echo "package: staging git-tracked files into ${STAGE_DIR}"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

git ls-files -z | while IFS= read -r -d '' f; do
    mkdir -p "$STAGE_DIR/$(dirname "$f")"
    cp "$f" "$STAGE_DIR/$f"
done

# The sample config must ship (placeholders only, no real keys).
if [ ! -f "$STAGE_DIR/config/gateway.example.json" ]; then
    echo "package: ERROR config/gateway.example.json missing from stage" >&2
    exit 1
fi

# Safety net: refuse to package anything that looks like a live credential.
if find "$STAGE_DIR" -type f \( -name '*.secret' -o -name '*.s' -o -name '.env' \) -print -quit | grep -q .; then
    echo "package: ERROR secret-looking file found in stage, aborting" >&2
    exit 1
fi
if find "$STAGE_DIR/config" -type f -name 'gateway.json' -print -quit 2>/dev/null | grep -q .; then
    echo "package: ERROR live config/gateway.json found in stage, aborting" >&2
    exit 1
fi

echo "package: creating $(basename "$ARCHIVE")"
rm -f "$ARCHIVE"
if command -v zip >/dev/null 2>&1; then
    (cd "$ROOT/dist" && zip -qr "$(basename "$ARCHIVE")" rest_exchange_gateway)
else
    (cd "$ROOT/dist" && python3 -m zipfile -c "$ARCHIVE" rest_exchange_gateway)
fi

echo "package: done -> $ARCHIVE"
