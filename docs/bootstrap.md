# Bootstrap and self-hosting

This document states exactly what is written in what, so nobody has to guess.

## Build stages

```
stage 0   a C99 compiler                    (provided by your system, used once)
   │
   ▼
stage 1   bin/velac                          C  ~7,000 lines
          the bootstrap compiler: lexer, parser, resolver, type checker,
          monomorphiser, IR, optimiser, x86-64 encoder, ELF writer
   │
   │ compiles
   ▼
stage 2   lib/core + lib/std                 Vela  ~4,000 lines
          the runtime and standard library: allocator, garbage collector,
          strings, lists, maps, formatting, files, processes, sockets
   │
   │ compiles
   ▼
stage 3   bin/vela                           Vela  ~2,000 lines
          the toolchain: lexer, formatter, documentation generator,
          project driver, package manager, REPL
```

Reproducing it from a clean checkout:

```console
$ make clean && make
$ ./tests/run.sh
```

`make` is deterministic: the same sources produce byte-identical binaries.

## What is written in Vela today

| component | language | lines |
|-----------|----------|-------|
| memory allocator, size classes, chunk manager | **Vela** | ~250 |
| mark–sweep garbage collector | **Vela** | ~200 |
| strings, lists, maps | **Vela** | ~450 |
| big-integer arithmetic and exact float printing | **Vela** | ~300 |
| generic `str`, `==`, hashing over type descriptors | **Vela** | ~250 |
| `std/io`, `fs`, `os`, `path`, `math`, `rand`, `time`, `sort`, `json`, `net`, `process`, `fmt`, `testing` | **Vela** | ~2,500 |
| Vela lexer (second implementation) | **Vela** | ~400 |
| formatter | **Vela** | ~600 |
| documentation generator | **Vela** | ~250 |
| `vela` driver, manifest parser, lockfile, REPL | **Vela** | ~700 |
| lexer, parser, type checker, IR, backend | C | ~7,000 |

So: **the runtime, the standard library and the entire toolchain other than the
compiler itself are written in Vela.** Every Vela program you run — including
`vela fmt` and `vela doc` — executes native code produced by `velac` and calls
into a garbage collector written in Vela.

## What is *not* self-hosted

**Vela is not a self-hosted language yet.** The compiler's front end (parser,
name resolution, type checking, monomorphisation) and back end (IR, register
allocation, x86-64 encoding, ELF) are C. Rewriting them in Vela is the remaining
work, and this document will be updated when — and only when — `velac` can
compile itself.

We say this plainly because claiming self-hosting without it is the kind of thing
that makes a language impossible to trust.

## Why the second lexer matters

`tools/lex.vela` is an independent implementation of the same lexical
specification as `bootstrap/src/lex.c`. It exists for two reasons:

1. `vela fmt` and `vela doc` need to read Vela without depending on the compiler
   internals.
2. Two implementations of one specification disagree loudly. This has already
   found real bugs — the handling of `}}` inside a string interpolation is
   subtly different from `}}` outside one, and the second implementation is what
   surfaced it.

The formatter round-trips every source file in the repository on every test run,
which is a continuous consistency check between the two lexers.

## The path to self-hosting

The remaining work, in the order it should be done:

1. **Parser in Vela.** The lexer already exists. The AST maps naturally onto
   Vela enums, and the formatter already encodes most of the grammar. Verify by
   comparing the formatter's output before and after switching it to the new
   parser.
2. **Type checker in Vela.** The largest single piece. Verify by checking that
   it accepts and rejects exactly the same programs as `velac --check` across
   `tests/run/`, `tests/fail/`, `lib/` and `examples/`.
3. **IR and backend in Vela.** The IR is small and the encoder is mechanical.
   Verify by comparing emitted bytes with the C backend, function by function.
4. **The bootstrap triple.**
   * stage A: `velac` (C) compiles `velac2` (Vela) → binary **A**
   * stage B: **A** compiles `velac2` → binary **B**
   * stage C: **B** compiles `velac2` → binary **C**
   * self-hosting is achieved when **B** and **C** are byte-identical.

Two language features would make this materially easier and are the strongest
candidates for a 1.1: a way to grow a byte buffer without going through `[Byte]`,
and tagged unions with named fields so the AST needs fewer parallel arrays.

## Making the bootstrap reproducible

* `velac` links nothing but libc and reads nothing but the source files it is
  given.
* The output contains no timestamps, no paths and no build IDs, so the same
  input always produces the same executable.
* `make clean && make && sha256sum bin/velac bin/vela` is stable across machines
  with the same C compiler.
* Programs produced by `velac` are static: no dynamic loader, no libc, no
  version skew at run time.
