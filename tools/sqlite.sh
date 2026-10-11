#!/bin/sh
# Fetch the SQLite 3.53.4 amalgamation (sqlite3.c, shell.c and the headers)
# into $KCC_DATA (default ~/.local/share/kcc) for tests/run-sqlite.sh. Safe
# to rerun.
set -e
DATA=${KCC_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc}
mkdir -p "$DATA"
cd "$DATA"
if [ ! -f sqlite-amalgamation-3530400/sqlite3.c ]; then
    curl -sL -o sqlite-amalgamation-3530400.zip https://sqlite.org/2026/sqlite-amalgamation-3530400.zip
    unzip -qo sqlite-amalgamation-3530400.zip && rm sqlite-amalgamation-3530400.zip
fi
echo "done: $DATA/sqlite-amalgamation-3530400"
