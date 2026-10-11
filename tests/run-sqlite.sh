#!/bin/sh
# The third real-program milestone: build SQLite 3.53.4 (the amalgamation
# and its shell) with kcc and drive the shell through a database on disk.
# `sh tests/run-sqlite.sh` builds natively and runs it here;
# `sh tests/run-sqlite.sh x86_64` cross compiles and links a Linux binary
# here and runs it in Docker. Needs tools/sqlite.sh to have run. Run from
# the repository root.
cd "$(dirname "$0")/.." || exit 1
KCC=${KCC:-./kcc}
DATA=${KCC_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc}
SQLITE=$DATA/sqlite-amalgamation-3530400
[ -f "$SQLITE/sqlite3.c" ] && [ -f "$SQLITE/shell.c" ] || { echo "no SQLite at $DATA: run tools/sqlite.sh"; exit 2; }
mode=${1:-native}
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

# SQLite's default configuration: it multiplies with __uint128_t and, on
# the x86_64 target (which claims GCC), checks overflow with the
# __builtin_*_overflow functions.
FLAGS="-std=gnu99"
if [ "$mode" = x86_64 ]; then
    SYSROOT=${X86_64_SYSROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc/x86_64-linux-musl}
    docker info >/dev/null 2>&1 || { echo "docker is not running"; exit 2; }
    command -v ld.lld >/dev/null 2>&1 || { echo "no ld.lld: brew install lld"; exit 2; }
    X="$KCC --target=x86_64-linux-musl -isysroot $SYSROOT"
else
    X=$KCC
fi
echo "building sqlite3 ($mode)"
# shellcheck disable=SC2086
$X $FLAGS -o "$tmp/sqlite3" "$SQLITE/sqlite3.c" "$SQLITE/shell.c" -lm -ldl -lpthread \
    || { echo "FAIL: sqlite3 did not build"; exit 1; }

# the session: a table on disk, an index, a recursive CTE, then the file is
# reopened and checked (-init /dev/null keeps ~/.sqliterc out of it)
cat > "$tmp/session.sql" <<'SQL'
.headers off
.mode list
create table t(id integer primary key, name text, score real);
insert into t(name, score) values('ann', 1.5), ('bob', 2.25), ('cy', NULL);
create index i on t(name);
select name, score * 2, length(name) from t order by score desc;
with r(n) as (select 1 union all select n + 1 from r where n < 5) select group_concat(n) from r;
select count(*), avg(score), typeof(min(score)) from t;
select printf('%08.3f|%s', 3.14159, upper(name)) from t where name > 'a' order by name;
.schema
SQL
cat > "$tmp/want" <<'WANT'
bob|4.5|3
ann|3.0|3
cy||2
1,2,3,4,5
3|1.875|real
0003.142|ANN
0003.142|BOB
0003.142|CY
CREATE TABLE t(id integer primary key, name text, score real);
CREATE INDEX i on t(name);
ok
3
WANT
cat > "$tmp/go.sh" <<'GO'
./sqlite3 -version | head -1 || exit 1
./sqlite3 -init /dev/null -batch t.db < session.sql > out 2>&1 || exit 1
./sqlite3 -init /dev/null -batch t.db "pragma integrity_check; select count(*) from t;" >> out 2>&1
GO
if [ "$mode" = x86_64 ]; then
    docker run --rm --platform linux/amd64 -v "$tmp:/w" kcc-x86_64-linux sh -c "cd /w && sh go.sh"
    rc=$?
else
    (cd "$tmp" && sh go.sh)
    rc=$?
fi
if [ "$rc" -eq 0 ] && cmp -s "$tmp/want" "$tmp/out"; then
    echo "SQLITE OK ($mode)"
else
    echo "FAIL: the sqlite3 session did not go as expected ($mode)"
    diff "$tmp/want" "$tmp/out" | head -20
    exit 1
fi
