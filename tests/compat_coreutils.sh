#!/usr/bin/env bash
# LD_PRELOAD compatibility check for GNU coreutils and common shell workflows.
# Usage: compat_coreutils.sh /path/to/libhpmalloc.so
set -u

LIB="${1:-}"
if [[ -z "${LIB}" || ! -r "${LIB}" ]]; then
    echo "usage: $0 /path/to/libhpmalloc.so" >&2
    exit 2
fi

export LC_ALL=C
TMP="$(mktemp -d "${TMPDIR:-/tmp}/hpm-coreutils.XXXXXX")" || exit 2
trap 'rm -rf "${TMP}"' EXIT

PASS=0
FAIL=0
SKIP=0

pass() { PASS=$((PASS + 1)); }
skip() { SKIP=$((SKIP + 1)); }
fail() {
    FAIL=$((FAIL + 1))
    echo "FAIL: $*" >&2
}

run_rc() { # label expected_rc command...
    local label="$1" want="$2"
    shift 2
    timeout 30 env LD_PRELOAD="${LIB}" "$@" >"${TMP}/out" 2>"${TMP}/err"
    local rc=$?
    if [[ "${rc}" -eq "${want}" ]]; then
        pass
    else
        fail "${label} rc=${rc} want=${want} $(head -c 200 "${TMP}/err" | tr '\n' ' ')"
    fi
}

cmp_cmd() { # label command...
    local label="$1"
    shift
    timeout 60 "$@" </dev/null >"${TMP}/base.out" 2>"${TMP}/base.err"
    local rb=$?
    timeout 60 env LD_PRELOAD="${LIB}" "$@" </dev/null >"${TMP}/pre.out" 2>"${TMP}/pre.err"
    local rp=$?
    if [[ "${rb}" -eq "${rp}" ]] && cmp -s "${TMP}/base.out" "${TMP}/pre.out"; then
        pass
    else
        fail "${label} rc=${rb}/${rp}"
    fi
}

cmp_stdin() { # label input command...
    local label="$1" input="$2"
    shift 2
    "$@" <"${input}" >"${TMP}/base.out" 2>"${TMP}/base.err"
    local rb=$?
    timeout 60 env LD_PRELOAD="${LIB}" "$@" <"${input}" >"${TMP}/pre.out" 2>"${TMP}/pre.err"
    local rp=$?
    if [[ "${rb}" -eq "${rp}" ]] && cmp -s "${TMP}/base.out" "${TMP}/pre.out"; then
        pass
    else
        fail "${label} rc=${rb}/${rp}"
    fi
}

opts_check() { # label option command...
    local label="$1" opt="$2"
    shift 2
    timeout 60 env LD_PRELOAD="${LIB}" HPMALLOC_OPTIONS="${opt}" "$@" \
        >/dev/null 2>"${TMP}/err"
    if [[ $? -eq 0 ]]; then
        pass
    else
        fail "${label} option=${opt} $(head -c 160 "${TMP}/err" | tr '\n' ' ')"
    fi
}

# ---------------------------------------------------------------------------
# 1. Every installed coreutils binary starts under LD_PRELOAD.
# ---------------------------------------------------------------------------

COREUTILS=(
    '[' b2sum base32 base64 basename basenc cat chcon chgrp chmod chown chroot
    cksum comm cp csplit cut date dd df dir dircolors dirname du echo env expand
    expr factor false fmt fold groups head hostid id install join link ln logname
    ls md5sum mkdir mkfifo mknod mktemp mv nice nl nohup nproc numfmt od paste
    pathchk pr printenv printf ptx pwd readlink realpath rm rmdir runcon seq
    sha1sum sha224sum sha256sum sha384sum sha512sum shred shuf sleep sort split
    stat stdbuf stty sum sync tac tail tee test timeout touch tr true truncate
    tsort tty uname unexpand uniq unlink uptime users vdir wc who whoami yes
)

for c in "${COREUTILS[@]}"; do
    bin="$(type -P "${c}" 2>/dev/null || true)"
    if [[ -z "${bin}" ]]; then
        skip
        continue
    fi
    case "${c}" in
        '[')   run_rc "[ -f]" 0 "${bin}" -f /etc/hostname ']' ;;
        test)  run_rc "test -f" 0 "${bin}" -f /etc/hostname ;;
        false) run_rc "false" 1 "${bin}" ;;
        true)  run_rc "true" 0 "${bin}" ;;
        *)     run_rc "${c} --version" 0 "${bin}" --version ;;
    esac
done

# ---------------------------------------------------------------------------
# 2. Deterministic text processing: baseline vs preload output.
# ---------------------------------------------------------------------------

seq 1 200000 >"${TMP}/nums"
seq 1 5000 >"${TMP}/small"
printf 'alpha\nbeta\nalpha\ngamma\nbeta\n' >"${TMP}/words"
printf '1 a\n2 b\n3 c\n' >"${TMP}/j1"
printf '1 x\n3 z\n4 w\n' >"${TMP}/j2"
printf '\tfoo\tbar\n\tbaz\n' >"${TMP}/tabs"
printf 'b a\nc d\na e\n' >"${TMP}/graph"
dd if=/dev/zero of="${TMP}/big.bin" bs=1M count=8 status=none

cmp_cmd "sort" sort "${TMP}/nums"
cmp_cmd "sort -n" sort -n "${TMP}/nums"
cmp_cmd "sort -u" sort -u "${TMP}/words"
cmp_cmd "sort --parallel=4" sort --parallel=4 -n "${TMP}/nums"
cmp_cmd "sort -S64M" sort -S 64M -n "${TMP}/nums"
cmp_cmd "uniq" uniq "${TMP}/words"
cmp_cmd "uniq -c" uniq -c "${TMP}/words"
cmp_cmd "cut" cut -d: -f1 /etc/passwd
cmp_cmd "tac" tac "${TMP}/nums"
cmp_cmd "rev" rev "${TMP}/words"
cmp_cmd "fold" fold -w 20 /etc/passwd
cmp_cmd "nl" nl -ba "${TMP}/words"
cmp_cmd "cat" cat /etc/hostname
cmp_cmd "head" head -n 100 "${TMP}/nums"
cmp_cmd "tail" tail -n 100 "${TMP}/nums"
cmp_cmd "wc" wc -l -c /etc/passwd
cmp_cmd "od" od -An -tx1 -N64 /etc/hostname
cmp_cmd "base64" base64 /etc/hostname
cmp_cmd "base32" base32 /etc/hostname
cmp_cmd "basenc b64url" basenc --base64url /etc/hostname
cmp_cmd "b2sum" b2sum /etc/hostname
cmp_cmd "md5sum" md5sum /etc/hostname
cmp_cmd "sha256sum" sha256sum /etc/hostname
cmp_cmd "cksum" cksum /etc/hostname
cmp_cmd "sum" sum /etc/hostname
cmp_cmd "paste" paste "${TMP}/j1" "${TMP}/j2"
cmp_cmd "join" join "${TMP}/j1" "${TMP}/j2"
cmp_cmd "comm" comm "${TMP}/j1" "${TMP}/j2"
cmp_cmd "expand" expand "${TMP}/tabs"
cmp_cmd "unexpand" unexpand -a "${TMP}/tabs"
cmp_cmd "pr" pr -t "${TMP}/words"
cmp_cmd "fmt" fmt -w 20 "${TMP}/words"
cmp_cmd "ptx" ptx -t "${TMP}/words"
cmp_cmd "tsort" tsort "${TMP}/graph"
cmp_cmd "seq" seq 1 1000
cmp_cmd "seq -f" seq -f 'x%g' 1 100
cmp_cmd "factor" factor 123456789
cmp_cmd "numfmt" numfmt --to=iec 1048576
cmp_cmd "expr" expr 123456789 '*' 987654321
cmp_cmd "dircolors" dircolors -b
cmp_cmd "printf" printf '%08x %s %c\n' 255 abc XYZ
cmp_cmd "echo" echo hello world
cmp_cmd "shuf" shuf --random-source="${TMP}/nums" "${TMP}/small"
cmp_cmd "ls -l" ls -l /etc/hostname
cmp_cmd "stat" stat /etc/hostname
cmp_cmd "du file" du -s /etc/hostname
run_rc "df" 0 df -h /tmp
cmp_cmd "realpath" realpath /etc/hostname
cmp_cmd "readlink" readlink /proc/self/exe
cmp_cmd "basename" basename /a/b/c
cmp_cmd "dirname" dirname /a/b/c
cmp_stdin "tr stdin" /etc/hostname tr a-z A-Z

run_rc "date" 0 date +%Y-%m-%d
run_rc "uname" 0 uname -a
run_rc "id" 0 id
run_rc "whoami" 0 whoami
run_rc "nproc" 0 nproc
run_rc "groups" 0 groups
run_rc "hostid" 0 hostid
run_rc "logname" 0 logname
run_rc "users" 0 users
run_rc "who" 0 who
run_rc "uptime" 0 uptime
run_rc "env -i" 0 env -i PATH=/usr/bin /usr/bin/true
run_rc "nice" 0 nice -n 5 /usr/bin/true
run_rc "stdbuf" 0 stdbuf -o0 /usr/bin/true
run_rc "nohup" 0 nohup /usr/bin/true
run_rc "timeout" 0 timeout 5 /usr/bin/true
run_rc "timeout 124" 124 timeout 0.2 sleep 5
run_rc "sleep" 0 sleep 0.05
run_rc "sync" 0 sync
run_rc "pathchk" 0 pathchk /etc/hostname
run_rc "printenv" 0 printenv PATH

# ---------------------------------------------------------------------------
# 3. File operations: same operations in two empty directories, compare trees.
# ---------------------------------------------------------------------------

printf 'A\tB\tC\nD\tE\tF\n' >"${TMP}/input.tsv"
mkdir -p "${TMP}/base" "${TMP}/pre"
cat >"${TMP}/ops.sh" <<'OPS'
set -u
d="$1"
cd "$d" || exit 1
cp ../nums a.txt || exit 1
cp ../big.bin b.bin || exit 1
cp ../input.tsv t.tsv || exit 1
mkdir sub || exit 1
cp a.txt sub/a2.txt || exit 1
cp -r sub sub2 || exit 1
mv sub2/a2.txt moved.txt || exit 1
ln a.txt hard.txt || exit 1
ln -s a.txt soft.txt || exit 1
link a.txt link2.txt || exit 1
unlink link2.txt || exit 1
touch touched.txt || exit 1
chmod 640 touched.txt || exit 1
truncate -s 12345 trunc.txt || exit 1
fallocate -l 1048576 falloc.bin || exit 1
split -b 1M b.bin part. || exit 1
csplit -s -f cs- ../nums 100000 || exit 1
printf 'one\ntwo\nthree\n' > tee.txt || exit 1
printf 'tee-line\n' | tee -a tee.txt >/dev/null || exit 1
dd if=../big.bin of=dd.bin bs=1M count=4 status=none || exit 1
install -m 600 a.txt installed.txt || exit 1
mkfifo fifo || exit 1
sort -n ../nums -o sorted.txt || exit 1
shuf --random-source=../nums a.txt -o shuffled.txt || exit 1
head -n 1000 sorted.txt > h.txt || exit 1
tail -n 1000 sorted.txt > t.txt || exit 1
sed -n '1,100p' sorted.txt > sed.txt || exit 1
awk 'NR<=100 {print $1}' sorted.txt > awk.txt || exit 1
grep -n '^12345$' sorted.txt > grep.txt || exit 1
cp a.txt shred.bin || exit 1
shred -n 1 -z -s 4096 shred.bin || exit 1
rm -f hard.txt link2.txt || exit 1
OPS

( cd "${TMP}/base" && bash "${TMP}/ops.sh" "${TMP}/base" ) >"${TMP}/base.log" 2>&1
rb=$?
( export LD_PRELOAD="${LIB}"; cd "${TMP}/pre" && bash "${TMP}/ops.sh" "${TMP}/pre" ) >"${TMP}/pre.log" 2>&1
rp=$?
if [[ ${rb} -eq 0 && ${rp} -eq 0 ]]; then
    ( cd "${TMP}/base" && find . -mindepth 1 -printf '%y %m %p %l\n' | sort ) >"${TMP}/base.list"
    ( cd "${TMP}/pre"  && find . -mindepth 1 -printf '%y %m %p %l\n' | sort ) >"${TMP}/pre.list"
    ( cd "${TMP}/base" && find . -type f -print0 | sort -z | xargs -0 sha256sum ) >"${TMP}/base.sha"
    ( cd "${TMP}/pre"  && find . -type f -print0 | sort -z | xargs -0 sha256sum ) >"${TMP}/pre.sha"
    if cmp -s "${TMP}/base.list" "${TMP}/pre.list" && cmp -s "${TMP}/base.sha" "${TMP}/pre.sha"; then
        pass
    else
        fail "file operations tree differs"
        diff -u "${TMP}/base.list" "${TMP}/pre.list" | head -20 >&2
        diff -u "${TMP}/base.sha" "${TMP}/pre.sha" | head -20 >&2
    fi
else
    fail "file operations rc=${rb}/${rp}"
    tail -20 "${TMP}/base.log" >&2
    tail -20 "${TMP}/pre.log" >&2
fi

# ---------------------------------------------------------------------------
# 4. Archive/compression round trips (tools are not required; skip if absent).
# ---------------------------------------------------------------------------

mkdir -p "${TMP}/src/sub"
seq 1 100000 >"${TMP}/src/nums"
head -c 4194304 /dev/zero | tr '\0' 'A' >"${TMP}/src/sub/big.txt"
if command -v tar >/dev/null; then
    mkdir -p "${TMP}/tarx"
    if env LD_PRELOAD="${LIB}" tar -cf "${TMP}/pre.tar" -C "${TMP}/src" . 2>"${TMP}/err" &&
       tar -xf "${TMP}/pre.tar" -C "${TMP}/tarx" 2>>"${TMP}/err" &&
       diff -r "${TMP}/src" "${TMP}/tarx" >/dev/null 2>&1; then
        pass
    else
        fail "tar round trip"
    fi
else
    skip
fi

for comp in gzip bzip2 xz zstd; do
    if ! command -v "${comp}" >/dev/null; then
        skip
        continue
    fi
    if env LD_PRELOAD="${LIB}" "${comp}" -c "${TMP}/nums" >"${TMP}/f.${comp}" 2>"${TMP}/err" &&
       "${comp}" -dc <"${TMP}/f.${comp}" >"${TMP}/o.${comp}" 2>>"${TMP}/err" &&
       cmp -s "${TMP}/nums" "${TMP}/o.${comp}"; then
        pass
    else
        fail "${comp} round trip"
    fi
done

# ---------------------------------------------------------------------------
# 5. Shell workflows, fork/exec, pipelines, and option variants.
# ---------------------------------------------------------------------------

cmp_cmd "bash pipeline" bash -c 'seq 1 200000 | sort -n | tail -n 1'
cmp_cmd "bash command substitution" bash -c 'x=$(seq 1 100); echo "$x"'
cmp_cmd "bash fork loop" bash -c 'i=0; while [ $i -lt 100 ]; do /bin/true; i=$((i+1)); done; echo ok'
cmp_cmd "yes|head" bash -c 'yes | head -n 3'
cmp_cmd "sort|uniq|wc" bash -c 'seq 1 50000 | sort -n | uniq | wc -l'

opts_check "option thp=always" "thp=always" sort -n "${TMP}/nums"
opts_check "option thp=never" "thp=never" sort -n "${TMP}/nums"
opts_check "option purge=dontneed" "purge=dontneed" sort -n "${TMP}/nums"
opts_check "option purge=defer" "purge=defer" sort -n "${TMP}/nums"
opts_check "option thp=collapse" "thp=collapse" sort -n "${TMP}/nums"
opts_check "option arena_mb=4" "arena_mb=4" sort -n "${TMP}/nums"
opts_check "option arena_mb=16" "arena_mb=16" sort -n "${TMP}/nums"

# ---------------------------------------------------------------------------

if [[ ${FAIL} -eq 0 ]]; then
    echo "compat_coreutils: PASS (pass=${PASS}, skip=${SKIP})"
    exit 0
fi
echo "compat_coreutils: FAIL (pass=${PASS}, fail=${FAIL}, skip=${SKIP})" >&2
exit 1
