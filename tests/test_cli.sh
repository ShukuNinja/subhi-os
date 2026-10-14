#!/usr/bin/env bash
set -euo pipefail

PROGRAM="./build/subhi"
IMAGE="build/test_cli.img"
trap 'rm -f "$IMAGE"' EXIT

printf '[1/5] CLI help/version and invalid command\n'
"$PROGRAM" --help > /dev/null
VERSION_OUTPUT=$("$PROGRAM" --version)
[[ "$VERSION_OUTPUT" == "Subhi-OS v0.3.0" ]]
if "$PROGRAM" --invalid > /dev/null 2>&1; then
    echo 'FAIL: invalid option should return nonzero'
    exit 1
fi

printf '[2/5] Format and basic file operations across separate invocations\n'
"$PROGRAM" format "$IMAGE" 1024 > /dev/null
"$PROGRAM" mkdir "$IMAGE" /docs > /dev/null
"$PROGRAM" create "$IMAGE" /docs/notes.txt > /dev/null
"$PROGRAM" write "$IMAGE" /docs/notes.txt 'persistent CLI data' > /dev/null
READ_OUTPUT=$("$PROGRAM" read "$IMAGE" /docs/notes.txt)
[[ "$READ_OUTPUT" == 'persistent CLI data' ]]

printf '[3/5] Directory listing and storage statistics\n'
LS_OUTPUT=$("$PROGRAM" ls "$IMAGE" /docs)
[[ "$LS_OUTPUT" == *'notes.txt'* ]]
STATS_OUTPUT=$("$PROGRAM" stats "$IMAGE")
[[ "$STATS_OUTPUT" == *'Allocated data blocks:'* ]]
[[ "$STATS_OUTPUT" == *'Physical block I/O:'* ]]
[[ "$STATS_OUTPUT" == *'Block cache:'* ]]

printf '[4/5] Integrity and non-empty directory protection\n'
"$PROGRAM" check "$IMAGE" > /dev/null
if "$PROGRAM" rmdir "$IMAGE" /docs > /dev/null 2>&1; then
    echo 'FAIL: rmdir should reject a non-empty directory'
    exit 1
fi

printf '[5/5] Delete file/directory and re-check integrity\n'
"$PROGRAM" rm "$IMAGE" /docs/notes.txt > /dev/null
"$PROGRAM" rmdir "$IMAGE" /docs > /dev/null
"$PROGRAM" check "$IMAGE" > /dev/null

echo 'All CLI integration tests passed.'
