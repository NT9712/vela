# Performance

Numbers from `./bench/run.sh` on a 2-core Intel Xeon at 2.20 GHz (a shared
cloud VM, so absolute times are noisy — ratios are the useful part). Reproduce
with:

```console
$ ./bench/run.sh
```

## Compiler

| workload | time |
|----------|------|
| `hello.vela`, including the whole standard library | **130–210 ms** |
| a 16,000-line, 2,000-function file | **570–1,300 ms** |
| the same file, `--check` only | **400–500 ms** |

Phase breakdown for the 16k-line file:

```
parse   ~200 ms      lex + parse, 100k tokens
sema    ~170 ms      resolve, type check, monomorphise
ir      ~165 ms      lowering, verification, optimisation
codegen  ~50 ms      register allocation, encoding, ELF
arena    ~30 MB      total compiler memory
```

Roughly **25,000–30,000 lines per second**, single threaded, whole program,
from source to executable with no linker in the loop. There is no incremental
or parallel compilation: every build re-reads and re-checks the standard
library, which costs about 25 ms.

## Executables

| metric | value |
|--------|-------|
| hello-world binary | **18 KB**, static, no interpreter, no libc |
| syscalls before `main` | **3** (`mmap` for the heap, the mark stack, the chunk table) |
| process start to `main` | **~1.4 ms** measured through `fork`+`exec` |

For scale, a dynamically linked `/bin/true` on the same machine measured
2.5 ms through the same harness — most of which is the loader Vela does not have.

## Runtime

| benchmark | time |
|-----------|------|
| `fib(30)` — 1.6M recursive calls | **77 ms** |
| n-body, 100,000 steps, 5 bodies | **1.22 s** |
| push 200,000 integers onto a list | **45 ms** |
| heapsort 200,000 integers through a closure | **1.09 s** |
| insert 100,000 string keys into a map | **1.4–1.8 s** |
| look up 100,000 string keys | **0.7–1.0 s** |
| join 20,000 strings | **46 ms** |
| allocate a 32-byte object | **~250 ns** |
| empty loop iteration | **~10 ns** |

For calibration, the same empty loop written in C and compiled at `-O0` — which
also keeps locals in memory — runs at 10 ns per iteration on this machine. Vela
matches unoptimised C on integer loop code and is well behind optimised C, which
is exactly what the implementation predicts.

## Garbage collector

From the collections benchmark (200,000 list elements, 100,000 map entries,
20,000 joined strings):

```
gc: 4 collections, 11 MiB live, 29 MiB heap
```

The threshold is kept at twice the live size with a 4 MiB floor, so steady-state
programs collect rarely. Collections are not incremental: a collection pauses for
O(live) marking plus O(heap) sweeping.

## What made the difference

Each of these came out of the benchmark suite finding something embarrassing:

| change | effect |
|--------|--------|
| a string builder instead of repeated `+` in `join` and the formatter | join: 75 s → 46 ms; formatting `core.vela`: 9.5 s → 0.5 s |
| power-of-two size classes, removing four integer divisions from `alloc` | allocation: 2.3 µs → 250 ns |
| bounds-checked list and string indexing expanded inline instead of called | heapsort: 3.7 s → 1.1 s |
| fusing a comparison into the branch that consumes it | empty loop: 45 ns → 10 ns |
| eliminating jumps to the next block | ~5% across the board |
| strength-reducing `/` and `%` by constant powers of two | bitmap-heavy runtime code |
| inlining constants that fold to literals | removed a load per use |

## Where the remaining cost is

Being specific about this is more useful than claiming it is fast.

1. **Locals live in frame slots, not registers.** Only expression temporaries get
   registers, and only the five callee-saved ones. Every named variable read is a
   load and every write is a store. Promoting non-captured locals to virtual
   registers with live ranges across blocks is the single largest remaining win,
   and would need either SSA or a cross-block live-range analysis.

2. **There is no inliner.** `size_class`, `bm_set`, `rtl` and friends are real
   calls. An IR-level inliner for small leaf functions would speed the runtime up
   noticeably.

3. **String keys are built before every map operation.** `m["key-{i}"]`
   allocates a string, formats an integer, and concatenates twice before the map
   is even consulted. That is a language-level cost, not a map cost — the map
   itself hashes 100,000 keys in 27 ms.

4. **Every fallible call allocates.** `!T` is always a heap object. A tagged
   representation that avoids allocating for `ok(v)` when `v` is a reference
   would remove most of it.

5. **No incremental compilation.** Every build re-parses the standard library.

None of these are hard to fix; they are simply not done, and the documentation
says so rather than implying otherwise.
