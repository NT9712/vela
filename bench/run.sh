#!/usr/bin/env bash
# bench/run.sh — measure the compiler and the code it produces.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export VELA_ROOT="$ROOT"
VELAC="$ROOT/bin/velac"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

hr() { printf '%s\n' "-------------------------------------------------------------"; }
timeit() { local s=$(date +%s%N); "$@" >/dev/null 2>&1; local e=$(date +%s%N); echo $(( (e-s)/1000000 )); }

echo "compiler"; hr
printf 'fn main() {\n    println("hi")\n}\n' > "$TMP/hello.vela"
python3 -c "
import sys
out=[]
for i in range(2000):
    out.append('fn f%d(a: Int, b: Int) -> Int {\n    let c = a * b + %d\n    if c > 100 {\n        return c - 1\n    }\n    return c\n}\n' % (i,i))
out.append('fn main() {\n    let mut t = 0\n')
for i in range(2000):
    out.append('    t += f%d(%d, 3)\n' % (i,i))
out.append('    println(str(t))\n}\n')
open('$TMP/big.vela','w').write(''.join(out))
"
wc -l "$TMP/big.vela" | awk '{printf "  synthetic source: %s lines\n", $1}'
# warm the page cache and the branch predictors first
for i in 1 2 3 4 5; do "$VELAC" -q -o "$TMP/h" "$TMP/hello.vela" >/dev/null 2>&1; done
for i in 1 2; do "$VELAC" -q -o "$TMP/b" "$TMP/big.vela" >/dev/null 2>&1; done
h=$(timeit "$VELAC" -q -o "$TMP/h" "$TMP/hello.vela")
b=$(timeit "$VELAC" -q -o "$TMP/b" "$TMP/big.vela")
c=$(timeit "$VELAC" -q --check "$TMP/big.vela")
echo "  hello.vela (with stdlib)       ${h} ms"
echo "  2000-function file             ${b} ms"
echo "  2000-function file (check)     ${c} ms"
"$VELAC" -q --time -o "$TMP/b" "$TMP/big.vela" 2>&1 | sed 's/^/  /'
echo "  hello binary size:             $(stat -c%s "$TMP/h") bytes"
echo

echo "startup"; hr
python3 - "$TMP/h" <<'PY'
import subprocess, sys, time
exe = sys.argv[1]
for _ in range(50): subprocess.run([exe], stdout=subprocess.DEVNULL)
t = time.time()
for _ in range(500): subprocess.run([exe], stdout=subprocess.DEVNULL)
d = (time.time() - t) / 500 * 1e6
t = time.time()
for _ in range(500): subprocess.run(["/bin/true"])
b = (time.time() - t) / 500 * 1e6
print("  vela hello:                    %.0f us/exec" % d)
print("  /bin/true (dynamic, for scale) %.0f us/exec" % b)
PY
echo

echo "runtime"; hr
for b in fib nbody collections; do
  "$VELAC" -q -o "$TMP/$b" "$ROOT/bench/$b.vela" || continue
done
"$TMP/fib" 30 | sed 's/^/  /'
"$TMP/nbody" 100000 | tail -1 | sed 's/^/  /'
"$TMP/collections" 200000 | sed 's/^/  /'
echo

echo "toolchain"; hr
if [ -x "$ROOT/bin/vela" ]; then
  cp "$ROOT/lib/core/core.vela" "$TMP/f.vela"
  t=$(timeit "$ROOT/bin/vela" fmt "$TMP/f.vela")
  echo "  format lib/core/core.vela:     ${t} ms  ($(wc -l < "$TMP/f.vela") lines)"
fi
