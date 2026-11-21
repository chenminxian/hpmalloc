#!/usr/bin/env bash
# LD_PRELOAD compatibility checks for common developer tools and runtime edges.
# Usage: compat_common.sh /path/to/libhpmalloc.so
set -u

LIB="${1:-}"
if [[ -z "${LIB}" || ! -r "${LIB}" ]]; then
    echo "usage: $0 /path/to/libhpmalloc.so" >&2
    exit 2
fi

export LC_ALL=C
TMP="$(mktemp -d "${TMPDIR:-/tmp}/hpm-common.XXXXXX")" || exit 2
trap 'rm -rf "${TMP}"' EXIT

PASS=0
FAIL=0
SKIP=0
pass() { PASS=$((PASS + 1)); }
skip() { SKIP=$((SKIP + 1)); }
fail() { FAIL=$((FAIL + 1)); echo "FAIL: $*" >&2; }

run_expect() { # label expected_rc command...
    local label="$1" want="$2"
    shift 2
    timeout 180 env LD_PRELOAD="${LIB}" "$@" >"${TMP}/out" 2>"${TMP}/err"
    local rc=$?
    if [[ "${rc}" -eq "${want}" ]]; then
        pass
    else
        fail "${label} rc=${rc} want=${want} $(head -c 200 "${TMP}/err" | tr '\n' ' ')"
    fi
}

have() { command -v "$1" >/dev/null 2>&1; }

# ---------------------------------------------------------------------------
# C / C++ compile and run under preload (compiler toolchain allocates heavily).
# ---------------------------------------------------------------------------

cat >"${TMP}/test.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    size_t n = 4u * 1024u * 1024u + 17u;
    unsigned char* p = malloc(n);
    if (!p) return 2;
    memset(p, 0x5a, n);
    if (p[0] != 0x5a || p[n - 1] != 0x5a) return 3;
    p = realloc(p, n * 2 + 7);
    if (!p) return 4;
    if (p[0] != 0x5a) return 5;
    free(p);
    puts("C_PRELOAD_PASS");
    return 0;
}
EOF

cat >"${TMP}/test.cpp" <<'EOF'
#include <cstdio>
#include <new>
#include <string>
#include <thread>
#include <vector>
struct alignas(64) A { unsigned char b[128]; };
static void worker() {
    for (int i = 0; i < 20000; ++i) {
        size_t n = 1 + static_cast<size_t>(i % 4096);
        unsigned char* p = new unsigned char[n];
        p[0] = 1; p[n - 1] = 2;
        delete[] p;
    }
}
int main() {
    std::vector<unsigned char> v(3u * 1024u * 1024u + 9u, 0x33);
    for (int i = 0; i < 100000; ++i) v.push_back(static_cast<unsigned char>(i));
    std::string s(100000, 'x');
    A* a = new A;
    if (reinterpret_cast<unsigned long>(a) % 64 != 0) return 2;
    delete a;
    std::thread t1(worker), t2(worker);
    t1.join(); t2.join();
    void* p = operator new(128, std::nothrow);
    if (!p) return 3;
    operator delete(p, std::nothrow);
    std::printf("CPP_PRELOAD_PASS %zu %zu\n", v.size(), s.size());
    return 0;
}
EOF

if have gcc; then
    if timeout 180 env LD_PRELOAD="${LIB}" gcc -O2 -o "${TMP}/test_c" "${TMP}/test.c" \
        >/dev/null 2>"${TMP}/err" &&
        timeout 60 env LD_PRELOAD="${LIB}" "${TMP}/test_c" >"${TMP}/out" 2>>"${TMP}/err" &&
        grep -q C_PRELOAD_PASS "${TMP}/out"; then
        pass
    else
        fail "gcc compile/run $(head -c 200 "${TMP}/err" | tr '\n' ' ')"
    fi
else
    skip
fi

if have g++; then
    if timeout 180 env LD_PRELOAD="${LIB}" g++ -O2 -std=c++2b -pthread \
        -o "${TMP}/test_cpp" "${TMP}/test.cpp" >/dev/null 2>"${TMP}/err" &&
       timeout 120 env LD_PRELOAD="${LIB}" "${TMP}/test_cpp" >"${TMP}/out" 2>>"${TMP}/err" &&
       grep -q CPP_PRELOAD_PASS "${TMP}/out"; then
        pass
    else
        fail "g++ compile/run $(head -c 200 "${TMP}/err" | tr '\n' ' ')"
    fi
else
    skip
fi

# ---------------------------------------------------------------------------
# Python: heavy containers, threads, subprocess/fork+exec.
# ---------------------------------------------------------------------------

if have python3; then
    cat >"${TMP}/test.py" <<'PY'
import hashlib, json, subprocess, threading
data = [str(i) * (i % 20) for i in range(100000)]
d = {i: data[i % len(data)] for i in range(100000)}
assert len(d) == 100000
blob = json.dumps({str(k): v for k, v in list(d.items())[:10000]}).encode()
assert len(hashlib.sha256(blob).hexdigest()) == 64
big = bytearray(16 * 1024 * 1024)
big[0] = 1; big[-1] = 2
def work():
    x = [bytearray(1024) for _ in range(2000)]
    assert len(x) == 2000
threads = [threading.Thread(target=work) for _ in range(4)]
for t in threads: t.start()
for t in threads: t.join()
r = subprocess.run(['/bin/true'], check=False)
assert r.returncode == 0
print("PY_PRELOAD_PASS")
PY
    run_expect "python3" 0 python3 "${TMP}/test.py"
    grep -q PY_PRELOAD_PASS "${TMP}/out" && pass || fail "python3 marker"
else
    skip
    skip
fi

# ---------------------------------------------------------------------------
# Git: init/add/commit/log/status in a temporary repository under preload.
# ---------------------------------------------------------------------------

if have git; then
    repo="${TMP}/gitrepo"
    mkdir -p "${repo}"
    if env LD_PRELOAD="${LIB}" bash -c "
        cd '${repo}' &&
        git init -q &&
        git config user.email test@example.com &&
        git config user.name test &&
        git config commit.gpgsign false &&
        seq 1 20000 > f.txt &&
        git add f.txt &&
        git commit -qm one &&
        git log --oneline | grep -q one &&
        git status --porcelain >/dev/null
    " >/dev/null 2>"${TMP}/err"; then
        pass
    else
        fail "git workflow $(head -c 200 "${TMP}/err" | tr '\n' ' ')"
    fi
else
    skip
fi

# ---------------------------------------------------------------------------
# CMake: version and a tiny configure (compiler detection/fork+exec).
# ---------------------------------------------------------------------------

if have cmake; then
    mkdir -p "${TMP}/tiny"
    cat >"${TMP}/tiny/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(tiny C)
add_executable(hello main.c)
EOF
    echo 'int main(void){return 0;}' >"${TMP}/tiny/main.c"
    if timeout 30 env LD_PRELOAD="${LIB}" cmake --version >/dev/null 2>"${TMP}/err" &&
       timeout 120 env LD_PRELOAD="${LIB}" cmake -S "${TMP}/tiny" -B "${TMP}/tiny/build" \
           >/dev/null 2>>"${TMP}/err"; then
        pass
    else
        fail "cmake $(head -c 200 "${TMP}/err" | tr '\n' ' ')"
    fi
else
    skip
fi

# ---------------------------------------------------------------------------
# SQLite / Perl
# ---------------------------------------------------------------------------

if have sqlite3; then
    if timeout 60 env LD_PRELOAD="${LIB}" sqlite3 :memory: \
        "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<50000) SELECT count(*), sum(x) FROM c;" \
        >"${TMP}/out" 2>"${TMP}/err" && grep -q '1250025000' "${TMP}/out"; then
        pass
    else
        fail "sqlite3"
    fi
else
    skip
fi

if have perl; then
    if timeout 60 env LD_PRELOAD="${LIB}" perl -e \
        'my @a; push @a, "x" x ($_ % 50) for 1..200000; my %h; $h{$_}=$a[$_] for 0..$#a; my $s=0; $s+=length($_) for values %h; print "PERL_PRELOAD_PASS $s\n";' \
        >"${TMP}/out" 2>"${TMP}/err" && grep -q PERL_PRELOAD_PASS "${TMP}/out"; then
        pass
    else
        fail "perl"
    fi
else
    skip
fi

# ---------------------------------------------------------------------------
# RLIMIT_AS fallback (our arena reservation must downsize gracefully).
# ---------------------------------------------------------------------------

for lim in 65536 131072 262144 1048576; do
    if timeout 30 bash -c "ulimit -v ${lim}; env LD_PRELOAD='${LIB}' /bin/ls /tmp >/dev/null" 2>"${TMP}/err"; then
        pass
    else
        fail "RLIMIT_AS ${lim} KiB"
    fi
done

# ---------------------------------------------------------------------------

if [[ ${FAIL} -eq 0 ]]; then
    echo "compat_common: PASS (pass=${PASS}, skip=${SKIP})"
    exit 0
fi
echo "compat_common: FAIL (pass=${PASS}, fail=${FAIL}, skip=${SKIP})" >&2
exit 1
