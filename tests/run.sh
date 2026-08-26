#!/usr/bin/env bash
# tests/run.sh — the Vela test suite.
#
#   ./tests/run.sh            run everything
#   ./tests/run.sh run        golden-output tests only
#   ./tests/run.sh fail       compile-failure tests only
#   ./tests/run.sh unit       `test` blocks in lib/ and examples/
#   ./tests/run.sh fuzz       randomised robustness testing
#
# Every test is self-describing: a `// EXPECT:` comment block at the top of a
# .vela file under tests/run/ holds the exact expected stdout, and a
# `// ERROR: <substring>` line in tests/fail/ names a diagnostic that must
# appear.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export VELA_ROOT="$ROOT"
VELAC="$ROOT/bin/velac"
VELA="$ROOT/bin/vela"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0; skipped=0
RED=$'\033[1;31m'; GRN=$'\033[1;32m'; YEL=$'\033[1;33m'; DIM=$'\033[2m'; OFF=$'\033[0m'
if [ ! -t 1 ]; then RED=; GRN=; YEL=; DIM=; OFF=; fi

ok()   { pass=$((pass+1)); printf "  %sok%s   %s\n" "$GRN" "$OFF" "$1"; }
bad()  { fail=$((fail+1)); printf "  %sFAIL%s %s\n" "$RED" "$OFF" "$1"; [ -n "${2:-}" ] && printf "%s%s%s\n" "$DIM" "$2" "$OFF"; }
skip() { skipped=$((skipped+1)); printf "  %sskip%s %s (%s)\n" "$YEL" "$OFF" "$1" "$2"; }

need_build() {
  if [ ! -x "$VELAC" ]; then
    echo "building the bootstrap compiler..."
    make -C "$ROOT" bin/velac >/dev/null || { echo "build failed"; exit 1; }
  fi
  if [ ! -x "$VELA" ]; then
    "$VELAC" -q --no-color -o "$VELA" "$ROOT/tools/cli.vela" >/dev/null 2>&1 || true
  fi
}

# ---------------------------------------------------------------- golden run

run_tests() {
  echo "golden output tests"
  for f in "$ROOT"/tests/run/*.vela; do
    name="$(basename "$f" .vela)"
    expected="$(sed -n 's|^// EXPECT:$||; t start; d; :start' "$f" >/dev/null; awk '/^\/\/ EXPECT:/{flag=1;next} /^\/\//{if(flag){sub(/^\/\/ ?/,"");print;next}} {if(flag)exit}' "$f")"
    if ! "$VELAC" -q --no-color -o "$TMP/$name" "$f" >"$TMP/$name.cerr" 2>&1; then
      bad "$name" "$(head -20 "$TMP/$name.cerr")"; continue
    fi
    actual="$(cd "$TMP" && timeout 20 "./$name" 2>&1)"
    if [ "$actual" = "$expected" ]; then ok "$name"
    else bad "$name" "$(diff <(echo "$expected") <(echo "$actual") | head -20)"; fi
  done
}

# ------------------------------------------------------------ compile errors

fail_tests() {
  echo "diagnostic tests"
  for f in "$ROOT"/tests/fail/*.vela; do
    name="$(basename "$f" .vela)"
    want="$(awk '/^\/\/ ERROR:/{sub(/^\/\/ ERROR: ?/,"");print}' "$f")"
    out="$("$VELAC" --no-color --check "$f" 2>&1)"
    rc=$?
    if [ $rc -eq 0 ]; then bad "$name" "compiled successfully but should not have"; continue; fi
    missing=""
    while IFS= read -r line; do
      [ -z "$line" ] && continue
      case "$out" in *"$line"*) ;; *) missing="$missing\n  wanted: $line";; esac
    done <<< "$want"
    if [ -z "$missing" ]; then ok "$name"
    else bad "$name" "$(printf "%b\n--- actual ---\n%s" "$missing" "$(echo "$out" | head -20)")"; fi
  done
}

# ------------------------------------------------------------------ unit

unit_tests() {
  echo "unit tests (\`test\` blocks)"
  for f in "$ROOT"/tests/lib/*.vela "$ROOT"/examples/calculator.vela \
           "$ROOT"/examples/wordcount.vela "$ROOT"/examples/http_server.vela; do
    [ -f "$f" ] || continue
    name="$(basename "$f" .vela)"
    if ! "$VELAC" -q --no-color --test -o "$TMP/t-$name" "$f" >"$TMP/t-$name.err" 2>&1; then
      bad "unit:$name" "$(head -20 "$TMP/t-$name.err")"; continue
    fi
    if out="$(timeout 60 "$TMP/t-$name" 2>&1)"; then
      n="$(echo "$out" | tail -1)"
      ok "unit:$name ${DIM}${n}${OFF}"
    else
      bad "unit:$name" "$out"
    fi
  done
}

# ------------------------------------------------------------------ toolchain

cli_tests() {
  echo "cli tests"
  [ -x "$VELA" ] || { skip "cli" "bin/vela not built"; return; }
  d="$TMP/proj"; mkdir -p "$d"; (cd "$d" && "$VELA" new app >/dev/null 2>&1)
  if [ ! -f "$d/app/vela.toml" ]; then bad "vela new"; return; fi
  ok "vela new"
  (cd "$d/app" && "$VELA" build >/dev/null 2>&1) && ok "vela build" || bad "vela build"
  out="$(cd "$d/app" && "$VELA" run 2>&1)"
  [ "$out" = "hello from app" ] && ok "vela run" || bad "vela run" "$out"
  out="$(cd "$d/app" && "$VELA" test 2>&1)"
  case "$out" in *"1 passed"*) ok "vela test";; *) bad "vela test" "$out";; esac
  (cd "$d/app" && "$VELA" check >/dev/null 2>&1) && ok "vela check" || bad "vela check"
  (cd "$d/app" && "$VELA" fmt >/dev/null 2>&1) && ok "vela fmt" || bad "vela fmt"
  (cd "$d/app" && "$VELA" fmt --check >/dev/null 2>&1) && ok "vela fmt --check" || bad "vela fmt --check"
  (cd "$d/app" && "$VELA" doc >/dev/null 2>&1) && ok "vela doc" || bad "vela doc"
  # dependency add / lockfile
  mkdir -p "$d/mathx/src"
  cat > "$d/mathx/vela.toml" <<'EOF'
[package]
name    = "mathx"
version = "0.2.0"
main    = "src/mathx.vela"
EOF
  cat > "$d/mathx/src/mathx.vela" <<'EOF'
pub fn triple(n: Int) -> Int {
    return n * 3
}
EOF
  (cd "$d/app" && "$VELA" add mathx --path ../mathx >/dev/null 2>&1) && ok "vela add" || bad "vela add"
  [ -f "$d/app/vela.lock" ] && ok "lockfile written" || bad "lockfile written"
  cat > "$d/app/src/main.vela" <<'EOF'
use std/io
use mathx/mathx

fn main() {
    io.println("{mathx.triple(14)}")
}
EOF
  out="$(cd "$d/app" && "$VELA" run 2>&1)"
  [ "$out" = "42" ] && ok "dependency resolution" || bad "dependency resolution" "$out"
  (cd "$d/app" && "$VELA" clean >/dev/null 2>&1) && ok "vela clean" || bad "vela clean"
}

# ------------------------------------------------------------------ formatter

fmt_tests() {
  echo "formatter tests"
  [ -x "$VELA" ] || { skip "fmt" "bin/vela not built"; return; }
  local bad_any=0
  for f in "$ROOT"/lib/core/*.vela "$ROOT"/lib/std/*.vela "$ROOT"/tools/*.vela \
           "$ROOT"/examples/*.vela "$ROOT"/tests/run/*.vela; do
    cp "$f" "$TMP/f.vela"
    "$VELA" fmt "$TMP/f.vela" >/dev/null 2>&1 || { bad "fmt $(basename $f)" "formatter errored"; bad_any=1; continue; }
    cp "$TMP/f.vela" "$TMP/f1.vela"
    "$VELA" fmt "$TMP/f.vela" >/dev/null 2>&1
    if ! diff -q "$TMP/f1.vela" "$TMP/f.vela" >/dev/null; then
      bad "fmt idempotent $(basename $f)" "$(diff "$TMP/f1.vela" "$TMP/f.vela" | head -10)"; bad_any=1
    fi
  done
  [ $bad_any -eq 0 ] && ok "formatter is idempotent on every source file"
}

# ------------------------------------------------------------------ fuzzing

regress_tests() {
  echo "regression corpus"
  local n=0 bad_any=0
  for f in "$ROOT"/tests/regress/*.vela; do
    [ -f "$f" ] || continue
    n=$((n+1))
    timeout 20 "$VELAC" --no-color --check "$f" >/dev/null 2>&1
    rc=$?
    if [ $rc -ne 0 ] && [ $rc -ne 1 ] && [ $rc -ne 2 ]; then
      bad "regress $(basename "$f")" "exit code $rc"; bad_any=1
    fi
  done
  [ $bad_any -eq 0 ] && ok "$n previously-crashing inputs now fail gracefully"
}

fuzz_tests() {
  echo "fuzz / robustness"
  local n=${FUZZ_N:-600}
  local crashes=0
  python3 - "$n" "$TMP" <<'PY'
import random, sys, os
n = int(sys.argv[1]); tmp = sys.argv[2]
random.seed(20260826)
frag = ["fn","main","(",")","{","}","let","mut","=","1","2.5",'"s"',"+","-","*","/","if","else",
        "while","for","in","match","=>",",",":","->","[","]","struct","enum","return","pub",
        "?","!","??","@","and","or","not","|","..","true","false","nil","use","std/io","test",
        "'a'","0x1f","self","const","type",".",";","<",">","==","_","T","Int","Str",'"{x}"',"\n"]
os.makedirs(tmp+"/fuzz", exist_ok=True)
for i in range(n):
    k = random.randint(1, 60)
    s = " ".join(random.choice(frag) for _ in range(k))
    open(f"{tmp}/fuzz/f{i}.vela","w").write(s)
PY
  for f in "$TMP"/fuzz/*.vela; do
    timeout 10 "$VELAC" --no-color --check "$f" >/dev/null 2>&1
    rc=$?
    # 0 = accepted, 1 = rejected with diagnostics, 2 = usage. Anything else
    # (segfault=139, abort=134, timeout=124, internal error=70) is a bug.
    if [ $rc -ne 0 ] && [ $rc -ne 1 ] && [ $rc -ne 2 ]; then
      crashes=$((crashes+1))
      cp "$f" "$ROOT/tests/fuzz-crash-$(basename $f)"
      bad "fuzz $(basename "$f")" "exit code $rc"
    fi
  done
  [ $crashes -eq 0 ] && ok "$(ls "$TMP"/fuzz | wc -l) random inputs handled without crashing"

  # structured mutation of real sources
  local mut=0
  python3 - "$ROOT" "$TMP" <<'PY'
import random, sys, os, glob
root, tmp = sys.argv[1], sys.argv[2]
random.seed(7)
os.makedirs(tmp+"/mut", exist_ok=True)
srcs = glob.glob(root+"/tests/run/*.vela") + glob.glob(root+"/examples/*.vela")
k = 0
for s in srcs:
    text = open(s).read()
    for _ in range(25):
        b = list(text)
        for _ in range(random.randint(1, 6)):
            i = random.randrange(len(b))
            op = random.random()
            if op < 0.4: b[i] = random.choice('{}()[]"\'<>,.;:=+-*/?!@|&')
            elif op < 0.7: b[i] = ''
            else: b.insert(i, random.choice('{}()[]"\n'))
        open(f"{tmp}/mut/m{k}.vela","w").write(''.join(b)); k += 1
PY
  for f in "$TMP"/mut/*.vela; do
    timeout 10 "$VELAC" --no-color --check "$f" >/dev/null 2>&1
    rc=$?
    if [ $rc -ne 0 ] && [ $rc -ne 1 ] && [ $rc -ne 2 ]; then
      mut=$((mut+1)); cp "$f" "$ROOT/tests/mut-crash-$(basename $f)"
      bad "mutation $(basename "$f")" "exit code $rc"
    fi
  done
  [ $mut -eq 0 ] && ok "$(ls "$TMP"/mut | wc -l) mutated sources handled without crashing"

  # pathological inputs
  python3 - "$TMP" <<'PY'
import sys
tmp = sys.argv[1]
open(tmp+"/deep.vela","w").write("fn main() {\n  let x = " + "("*300 + "1" + ")"*300 + "\n}\n")
open(tmp+"/deepblk.vela","w").write("fn main() {\n" + "if true {\n"*300 + "}\n"*300 + "}\n")
open(tmp+"/long.vela","w").write("fn main() {\n  let x = " + " + ".join(["1"]*20000) + "\n}\n")
open(tmp+"/huge.vela","w").write("\n".join(f"fn f{i}() -> Int {{ return {i} }}" for i in range(4000)) + "\nfn main() { println(str(f1())) }\n")
open(tmp+"/unterm.vela","w").write('fn main() { let s = "abc\n')
open(tmp+"/nul.vela","wb").write(b'fn main() { \x00 }')
open(tmp+"/bigint.vela","w").write("fn main() { let x = 999999999999999999999999 }\n")
open(tmp+"/emoji.vela","w").write('fn main() { println("héllo wörld 🎉") }\n')
PY
  for f in "$TMP"/deep.vela "$TMP"/deepblk.vela "$TMP"/long.vela "$TMP"/huge.vela \
           "$TMP"/unterm.vela "$TMP"/nul.vela "$TMP"/bigint.vela "$TMP"/emoji.vela; do
    timeout 60 "$VELAC" --no-color --check "$f" >/dev/null 2>&1
    rc=$?
    if [ $rc -ne 0 ] && [ $rc -ne 1 ] && [ $rc -ne 2 ]; then
      bad "pathological $(basename "$f")" "exit code $rc"
    else
      ok "pathological $(basename "$f")"
    fi
  done
}

need_build
what="${1:-all}"
case "$what" in
  run)  run_tests ;;
  regress) regress_tests ;;
  fail) fail_tests ;;
  unit) unit_tests ;;
  cli)  cli_tests ;;
  fmt)  fmt_tests ;;
  fuzz) fuzz_tests ;;
  *)    run_tests; fail_tests; unit_tests; fmt_tests; cli_tests; regress_tests; fuzz_tests ;;
esac

echo
printf "%d passed, %d failed" "$pass" "$fail"
[ $skipped -gt 0 ] && printf ", %d skipped" "$skipped"
echo
[ $fail -eq 0 ]
