#!/usr/bin/env bash
# Launches a one-shot Docker container that acts as an external client and
# runs the Phase 2 black-box suite (tests/blackbox/phase2_client_tests.sh)
# against a real gateway binary + scriptable fake OKX venue, both started
# inside that container. Nothing runs on the host except Docker.
#
# Usage: tests/blackbox/run_docker_client.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

cd "$ROOT"
docker compose build dev >/dev/null
exec docker compose run --rm --no-deps dev bash tests/blackbox/phase2_client_tests.sh
