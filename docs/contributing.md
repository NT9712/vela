# Contributing

## Layout

```
bootstrap/src/     the C bootstrap compiler
lib/core/          the runtime and prelude, in Vela
lib/std/           the standard library, in Vela
tools/             the lexer, formatter, doc generator and CLI, in Vela
examples/          runnable example programs
tests/             the test suite
docs/              this documentation
spec/SPEC.md       the authoritative language specification
editor/            syntax highlighting and the language server
bench/             benchmarks
```

## The loop

```console
$ make                 # rebuild velac and vela
$ ./tests/run.sh       # everything: golden output, diagnostics, unit, fmt, cli, fuzz
```

Run the suite before and after every change. It takes about a minute.

## Adding a language feature

A feature is not done until all six of these are true:

1. `spec/SPEC.md` describes it.
2. The parser accepts it and rejects near-misses with a useful message.
3. The type checker handles it, including its interaction with optionals,
   results, generics and pattern matching.
4. IR generation lowers it, and `ir_verify` passes.
5. `tests/run/` has a golden-output case and `tests/fail/` has a diagnostic case.
6. `docs/tour.md` shows it, and the formatter formats it idempotently.

If a feature makes any of these awkward, that is information about the feature.

## Adding a standard library function

* Write it in Vela under `lib/std/`.
* Give it a `///` doc comment: one sentence, imperative, saying what it returns.
* Return `?T` when absence is normal, `!T` when failure needs an explanation,
  and panic only for programmer error such as an out-of-range index.
* Add a case to `tests/lib/stdlib.vela`.
* Run `vela fmt`.

## Compiler invariants

These will bite you if you change codegen. They are checked by `ir_verify`, but
only if you run the tests.

1. **Emit operands before their consumers.**
2. **A virtual register never crosses a basic block.** If an expression can open
   blocks (`and`, `or`, `??`, `?`, `match`, map indexing) then anything computed
   before it must be parked in a frame slot — see `may_branch` and
   `gen_operands` in `irgen.c`.
3. **Evaluate every call argument before moving any into an argument register.**
   Otherwise a collection triggered by a later argument can free an earlier one.
4. **Only callee-saved registers are allocated to virtual registers.** This is
   what makes conservative stack scanning sound.
5. **Every expression carries a type after sema.**

## Debugging

```console
$ velac --emit-tokens f.vela     # the token stream
$ velac --emit-ir f.vela         # the optimised IR
$ velac --time f.vela            # per-phase timings and arena usage
```

For a crash in the compiler, build it with sanitizers:

```console
$ cc -g -O0 -fsanitize=address,undefined -std=c99 -w -o /tmp/velac bootstrap/src/*.c
$ VELA_ROOT=$PWD /tmp/velac --check broken.vela
```

For a crash in a *generated program*, first check whether it reproduces with the
collector effectively disabled by allocating less; if it does not, it is a GC
root problem, and the invariants above are the place to look.

## Style

* C: C99, four spaces, no tabs, no dependencies beyond `stdio`/`stdlib`/`string`.
* Vela: whatever `vela fmt` produces. There is nothing to argue about.
* Comments explain *why*. The code already says what.

## Regressions

Every bug that was found by fuzzing has its input saved under `tests/regress/`.
Every bug found any other way gets a case in `tests/run/` or `tests/fail/`.
Nothing is fixed without a test that would have caught it.
