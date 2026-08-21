#!/usr/bin/env bash
# Backward-compatible entry point: the live suite is venue-parameterized
# since it gained Binance support. See tests/live/live_func_tests.sh.
exec bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/live_func_tests.sh" okx "$@"
