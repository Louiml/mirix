#!/bin/bash
# Thin wrapper: the real boot test is tools/test_boot.py
cd "$(dirname "$0")/.."
exec python3 tools/test_boot.py "$@"
