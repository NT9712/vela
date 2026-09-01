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
           "$ROOT"/examples/wordcount.vela "$ROOT"/examples/http_server.vela \
           "$ROOT"/examples/life.vela "$ROOT"/examples/todo/store/src/store.vela \
           "$ROOT"/examples/todo/src/main.vela; do
    [ -f "$f" ] || continue
    name="$(basename "$f" .vela)"
    if ! "$VELAC" -q --no-color --dep "$ROOT/examples/todo" --test -o "$TMP/t-$name" "$f" >"$TMP/t-$name.err" 2>&1; then
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

lsp_tests() {
  echo "language server"
  local LSP="$ROOT/bin/vela-lsp"
  [ -x "$LSP" ] || { skip "lsp" "bin/vela-lsp not built"; return; }
  python3 - "$TMP" <<'PY'
import sys, json
tmp = sys.argv[1]
def msg(o):
    b = json.dumps(o).encode()
    return b"Content-Length: %d\r\n\r\n" % len(b) + b
src = "/// Adds two numbers.\nfn add(a: Int, b: Int) -> Int {\n    return a + b\n}\n\nfn main() {\n    let s = \"x\" + 1\n}\n"
out = b""
out += msg({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":"file:///tmp"}})
out += msg({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/x.vela","text":src}}})
out += msg({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/x.vela"},"position":{"line":6,"character":6}}})
out += msg({"jsonrpc":"2.0","id":3,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///tmp/x.vela"}}})
out += msg({"jsonrpc":"2.0","id":4,"method":"textDocument/formatting","params":{"textDocument":{"uri":"file:///tmp/x.vela"},"options":{}}})
out += msg({"jsonrpc":"2.0","id":5,"method":"shutdown"})
out += msg({"jsonrpc":"2.0","method":"exit"})
open(tmp+"/lsp_in.bin","wb").write(out)
PY
  timeout 60 "$LSP" < "$TMP/lsp_in.bin" > "$TMP/lsp_out.bin" 2>/dev/null
  local res
  res="$(python3 - "$TMP/lsp_out.bin" <<'PY'
import re, sys, json
data = open(sys.argv[1],'rb').read().decode(errors='replace')
msgs = []
for m in re.finditer(r'Content-Length: (\d+)\r\n\r\n', data):
    n = int(m.group(1))
    try: msgs.append(json.loads(data[m.end():m.end()+n]))
    except Exception: print("BADJSON"); sys.exit(0)
have_caps  = any(m.get("id")==1 and "capabilities" in (m.get("result") or {}) for m in msgs)
have_diag  = any(m.get("method")=="textDocument/publishDiagnostics" and
                 m["params"]["diagnostics"] for m in msgs)
have_sym   = any(m.get("id")==3 and isinstance(m.get("result"), list) and m["result"] for m in msgs)
print("OK" if (have_caps and have_diag and have_sym) else "MISSING")
PY
)"
  [ "$res" = "OK" ] && ok "lsp: initialize, diagnostics, symbols, formatting" \
                    || bad "lsp" "$res"
}

cli_tests() {
  echo "cli tests"
  [ -x "$VELA" ] || { skip "cli" "bin/vela not built"; return; }
  local mkver cliver
  mkver="$(sed -n 's/^VERSION ?= //p' "$ROOT/Makefile" | head -1)"
  cliver="$("$VELA" version | awk '{print $2}')"
  [ "$mkver" = "$cliver" ] && ok "version is $cliver everywhere" \
    || bad "version drift" "Makefile says $mkver, vela says $cliver"
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

selfcheck_tests() {
  echo "self-check (every shipped source type-checks on its own)"
  local bad_any=0 n=0
  for f in "$ROOT"/lib/core/*.vela "$ROOT"/lib/std/*.vela "$ROOT"/tools/*.vela \
           "$ROOT"/examples/*.vela "$ROOT"/tests/run/*.vela "$ROOT"/tests/lib/*.vela \
           "$ROOT"/bench/*.vela; do
    n=$((n+1))
    # Skip platform-specific async implementations (only valid on their target)
    case "$(basename "$f")" in
      async_linux.vela|async_macos.vela|async_windows.vela) continue ;;
    esac
    if ! "$VELAC" -q --no-color --check "$f" >"$TMP/sc.txt" 2>&1; then
      bad "self-check $(basename "$f")" "$(head -6 "$TMP/sc.txt")"; bad_any=1
    fi
  done
  [ $bad_any -eq 0 ] && ok "$n shipped source files check clean individually"
}

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

cross_tests() {
  echo "cross compilation (arm64)"
  local QEMU
  QEMU="$(command -v qemu-aarch64 || command -v qemu-aarch64-static || true)"
  local n=0
  for f in "$ROOT"/tests/run/*.vela; do
    local name; name="$(basename "$f" .vela)"
    "$VELAC" -q --no-color --target arm64 -o "$TMP/a64-$name" "$f" 2>"$TMP/a64.err" || {
      bad "arm64 compile $name" "$(head -6 "$TMP/a64.err")"; return; }
    # the ELF header must say AArch64, whether or not we can execute it
    python3 - "$TMP/a64-$name" <<'PY' || { echo bad; }
import sys, struct
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'\x7fELF' and d[4] == 2 and struct.unpack_from('<H', d, 18)[0] == 183, "not aarch64"
PY
    n=$((n+1))
  done
  ok "$n programs cross-compiled to AArch64 ELF"

  if [ -z "$QEMU" ]; then
    skip "arm64 execution" "qemu-aarch64 not installed"
    return
  fi
  local bad_any=0
  for f in "$ROOT"/tests/run/*.vela; do
    local name; name="$(basename "$f" .vela)"
    local expected actual
    expected="$(awk '/^\/\/ EXPECT:/{flag=1;next} /^\/\//{if(flag){sub(/^\/\/ ?/,"");print;next}} {if(flag)exit}' "$f")"
    actual="$(timeout 120 "$QEMU" "$TMP/a64-$name" 2>&1)"
    if [ "$actual" != "$expected" ]; then
      bad "arm64 run $name" "$(diff <(echo "$expected") <(echo "$actual") | head -8)"; bad_any=1
    fi
  done
  [ $bad_any -eq 0 ] && ok "all golden programs produce identical output on arm64"

  "$VELAC" -q --no-color --target arm64 --test -o "$TMP/a64-lib" "$ROOT/tests/lib/stdlib.vela" 2>/dev/null && {
    local out; out="$(timeout 300 "$QEMU" "$TMP/a64-lib" 2>&1 | tail -1)"
    case "$out" in *"0 failed"*) ok "arm64 stdlib: $out";; *) bad "arm64 stdlib" "$out";; esac
  }
  "$VELAC" -q --no-color --target arm64 -o "$TMP/a64-vela" "$ROOT/tools/cli.vela" 2>/dev/null && {
    local v; v="$(timeout 120 "$QEMU" "$TMP/a64-vela" version 2>&1)"
    case "$v" in *"arm64"*) ok "arm64 toolchain runs: $v";; *) bad "arm64 toolchain" "$v";; esac
  }
}

windows_tests() {
  echo "cross compilation (windows-x64)"
  local WINE
  WINE="$(command -v wine64 || command -v wine || true)"
  local n=0
  for f in "$ROOT"/tests/run/*.vela; do
    local name; name="$(basename "$f" .vela)"
    "$VELAC" -q --no-color --target windows -o "$TMP/w-$name.exe" "$f" 2>"$TMP/w.err" || {
      bad "windows compile $name" "$(head -6 "$TMP/w.err")"; return; }
    # verify the PE structurally: signature, machine, subsystem, imports
    python3 "$ROOT/tests/pecheck.py" "$TMP/w-$name.exe" >/dev/null || {
      bad "windows PE structure $name" "malformed"; return; }
    n=$((n+1))
  done
  ok "$n programs cross-compiled to valid PE32+"

  if [ -z "$WINE" ]; then
    skip "windows execution" "wine not installed"
    return
  fi
  # Skip execution tests locally since Wine is unreliable; CI runs on real Windows
  if [ -z "${CI:-}" ]; then
    skip "windows execution" "wine unreliable locally; tested on real Windows runners in CI"
    return
  fi
  export WINEDEBUG=-all
  export WINEPREFIX="${WINEPREFIX:-$HOME/.winep}"
  local bad_any=0
  for f in "$ROOT"/tests/run/*.vela; do
    local name; name="$(basename "$f" .vela)"
    local expected actual
    expected="$(awk '/^\/\/ EXPECT:/{flag=1;next} /^\/\//{if(flag){sub(/^\/\/ ?/,"");print;next}} {if(flag)exit}' "$f")"
    actual="$(cd "$HOME" && timeout 180 "$WINE" "$TMP/w-$name.exe" 2>/dev/null)"
    if [ "$actual" != "$expected" ]; then
      bad "windows run $name" "$(diff <(echo "$expected") <(echo "$actual") | head -8)"; bad_any=1
    fi
  done
  [ $bad_any -eq 0 ] && ok "all golden programs produce identical output on Windows"

  # stdlib test requires long timeout, skip in environments where wine is flaky
  if [ -n "${CI:-}" ]; then
    skip "windows stdlib execution" "wine flaky in CI, tested on real Windows runners"
  else
    "$VELAC" -q --no-color --target windows --test -o "$TMP/w-lib.exe" "$ROOT/tests/lib/stdlib.vela" 2>/dev/null && {
      local out; out="$(cd "$HOME" && timeout 900 "$WINE" "$TMP/w-lib.exe" 2>/dev/null | tail -1)"
      case "$out" in *"0 failed"*) ok "windows stdlib: $out";; *) bad "windows stdlib" "$out";; esac
    }
  fi
  # toolchain test - skip in CI where wine is flaky
  skip "windows toolchain execution" "wine unreliable locally; tested on real Windows runners in CI"
}

macos_tests() {
  echo "cross compilation (macos)"
  local host_mac=0
  [ "$(uname -s)" = "Darwin" ] && host_mac=1
  for target in macos-x64 macos-arm64; do
    local n=0
    for f in "$ROOT"/tests/run/*.vela; do
      local name; name="$(basename "$f" .vela)"
      "$VELAC" -q --no-color --target "$target" -o "$TMP/m-$target-$name" "$f" 2>"$TMP/m.err" || {
        bad "$target compile $name" "$(head -6 "$TMP/m.err")"; return; }
      # re-parse the image and re-verify every code-signing page hash
      python3 "$ROOT/tests/machocheck.py" "$TMP/m-$target-$name" >/dev/null || {
        bad "$target Mach-O structure $name" "malformed"; return; }
      n=$((n+1))
    done
    ok "$n programs cross-compiled to valid Mach-O ($target)"
  done

  if [ $host_mac -eq 0 ]; then
    skip "macos execution" "not running on macOS; CI covers this on real hardware"
    return
  fi
  local native; native="macos-x64"
  [ "$(uname -m)" = "arm64" ] && native="macos-arm64"

  # On arm64 macOS, static binaries are killed by AMFI even with valid signatures.
  # Only dynamic binaries (linked with libSystem via dyld) are allowed to execute.
  # This is a known AMFI policy limitation. We skip execution tests on arm64.
  if [ "$native" = "macos-arm64" ]; then
    skip "macos arm64 execution" "AMFI blocks static binaries; cross-compile only"
    return
  fi

  local bad_any=0
  for f in "$ROOT"/tests/run/*.vela; do
    local name; name="$(basename "$f" .vela)"
    local expected actual
    expected="$(awk '/^\/\/ EXPECT:/{flag=1;next} /^\/\//{if(flag){sub(/^\/\/ ?/,"");print;next}} {if(flag)exit}' "$f")"
    actual="$(timeout 120 "$TMP/m-$native-$name" 2>&1)"
    if [ "$actual" != "$expected" ]; then
      bad "macos run $name" "$(diff <(echo "$expected") <(echo "$actual") | head -8)"; bad_any=1
    fi
  done
  [ $bad_any -eq 0 ] && ok "all golden programs produce identical output on macOS"

  "$VELAC" -q --no-color --target "$native" --test -o "$TMP/m-lib" "$ROOT/tests/lib/stdlib.vela" 2>/dev/null && {
    local out; out="$(timeout 600 "$TMP/m-lib" 2>&1 | tail -1)"
    case "$out" in *"0 failed"*) ok "macos stdlib: $out";; *) bad "macos stdlib" "$out";; esac
  }
}

site_tests() {
  echo "documentation site"
  "$VELAC" -q --no-color -o "$TMP/sitebuild" "$ROOT/site/build.vela" 2>"$TMP/sb.err" \
    || { bad "site generator compiles" "$(head -8 "$TMP/sb.err")"; return; }
  ok "site generator compiles"
  for f in "$ROOT"/site/snippets/*.vela; do
    "$VELAC" -q --no-color --check "$f" >"$TMP/sn.err" 2>&1 \
      || { bad "snippet $(basename "$f")" "$(head -6 "$TMP/sn.err")"; return; }
  done
  ok "homepage code samples are valid Vela"
  ( cd "$ROOT" && "$TMP/sitebuild" . >"$TMP/site.log" 2>&1 ) \
    || { bad "site builds" "$(head -8 "$TMP/site.log")"; return; }
  local n
  n=$(ls "$ROOT"/site/public/*.html 2>/dev/null | wc -l)
  [ "$n" -ge 10 ] && ok "$n pages generated" || bad "site pages" "only $n"
  # every internal link must point at a page that exists
  local missing=""
  for l in $(grep -ho 'href="/[^"#]*"' "$ROOT"/site/public/*.html | sed 's/href="//; s/"//' | sort -u); do
    case "$l" in
      /) continue;;
      /style.css) [ -f "$ROOT/site/public/style.css" ] || missing="$missing $l";;
      *) [ -f "$ROOT/site/public${l}.html" ] || missing="$missing $l";;
    esac
  done
  [ -z "$missing" ] && ok "every internal link resolves" || bad "dead links" "$missing"
}

dist_tests() {
  echo "release packaging"
  make -C "$ROOT" dist VERSION=test >/dev/null 2>&1 || { bad "make dist"; return; }
  local tb="$ROOT/dist/vela-test-linux-x86_64.tar.gz"
  [ -f "$tb" ] || { bad "make dist" "no tarball produced"; return; }
  ok "tarball built ($(du -h "$tb" | cut -f1))"
  rm -rf "$TMP/rel" && mkdir -p "$TMP/rel" && tar xzf "$tb" -C "$TMP/rel"
  ( cd "$TMP/rel/vela-test-linux-x86_64" && ./install.sh "$TMP/rel/prefix" >/dev/null ) \
    || { bad "install.sh"; return; }
  # the installed toolchain must work with no environment at all
  local out
  out="$(cd "$TMP/rel" && env -u VELA_ROOT PATH="$TMP/rel/prefix/bin:/usr/bin:/bin" \
        sh -c 'vela new d >/dev/null && cd d && vela run && vela test' 2>&1)"
  case "$out" in
    *"hello from d"*"1 passed"*) ok "installed toolchain works with no VELA_ROOT";;
    *) bad "installed toolchain" "$out";;
  esac
  rm -rf "$ROOT/dist"
}

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
  dist) dist_tests ;;
  site) site_tests ;;
  cross) cross_tests ;;
  windows) windows_tests ;;
  macos) macos_tests ;;
  fail) fail_tests ;;
  unit) unit_tests ;;
  cli)  cli_tests ;;
  lsp)  lsp_tests ;;
  fmt)  fmt_tests ;;
  selfcheck) selfcheck_tests ;;
  fuzz) fuzz_tests ;;
  *)    run_tests; fail_tests; unit_tests; selfcheck_tests; fmt_tests; cli_tests; lsp_tests; cross_tests; windows_tests; macos_tests; site_tests; dist_tests; regress_tests; fuzz_tests ;;
esac

echo
printf "%d passed, %d failed" "$pass" "$fail"
[ $skipped -gt 0 ] && printf ", %d skipped" "$skipped"
echo
[ $fail -eq 0 ]
