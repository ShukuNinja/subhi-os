#!/usr/bin/env bash

set -euo pipefail

PROGRAM="./build/subhi"

echo "[1/3] Testing --help"
"$PROGRAM" --help > /dev/null

echo "[2/3] Testing --version"
VERSION_OUTPUT=$("$PROGRAM" --version)
[[ "$VERSION_OUTPUT" == "Subhi-OS v0.1.0" ]]

echo "[3/3] Testing invalid option"
if "$PROGRAM" --invalid > /dev/null 2>&1; then
    echo "FAIL: invalid option should return a nonzero status"
    exit 1
fi

echo "All CLI smoke tests passed."
