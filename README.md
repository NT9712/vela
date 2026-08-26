# Vela

**A small, fast, statically typed language that compiles straight to native
x86-64 executables — no VM, no libc, no linker, no runtime dependencies.**

```vela
use std/io

struct Point {
    x: Float,
    y: Float,
}

fn Point.length(self) -> Float {
    return (self.x * self.x + self.y * self.y).sqrt()
}

fn main() {
    let p = Point{ x: 3.0, y: 4.0 }
    io.println("|{p}| = {p.length()}")
}
```

```console
$ vela run
|Point{x: 3.0, y: 4.0}| = 5.0
```

---

## Why Vela exists

Most languages ask you to choose between *fast to write* and *fast to run*.
Vela's bet is that a language can be both if it stays small:

* **Simple** — the whole language is one document ([the specification](spec/SPEC.md)).
  Four primitive types, one reference model, no traits, no macros, no lifetimes,
  no implicit conversions.
* **Fast** — `velac` emits machine code directly. A hello-world binary is 10 KB,
  starts in under a millisecond, and makes exactly three syscalls before `main`.
* **Expressive** — closures, generics, sum types, pattern matching, string
  interpolation, and errors as values.
* **Cohesive** — the garbage collector, the string type, the collections, the
  formatter, the documentation generator and the `vela` command are all written
  *in Vela*. There is no privileged layer you cannot read.

---

## Install

Vela needs a C compiler to build the bootstrap compiler once. After that, the
toolchain is Vela all the way down.

```console
$ git clone <repo> vela && cd vela
$ make
$ export PATH="$PWD/bin:$PATH"
$ export VELA_ROOT="$PWD"
$ vela version
vela 1.0.0
```

`make` produces two binaries:

| binary      | what it is                                                    |
|-------------|---------------------------------------------------------------|
| `bin/velac` | the bootstrap compiler (C) — lexer through machine-code emitter |
| `bin/vela`  | the toolchain driver — **written in Vela and compiled by velac** |

---

## First five minutes

```console
$ vela new hello
created hello/

    cd hello
    vela run

$ cd hello && vela run
hello from hello

$ vela test
test greeting is not empty ... ok

1 passed, 0 failed, 1 total
```

---

## The language in one page

```vela
use std/io
use std/fs

const MAX: Int = 100                 // compile-time constant

struct User {                        // a reference type, garbage collected
    name: Str,
    age:  Int,
}

enum Shape {                         // a sum type
    Circle(Float),
    Rect(Float, Float),
    Empty,
}

fn area(s: Shape) -> Float {         // pattern matching, `match` is an expression
    return match s {
        Shape.Circle(r)  => 3.14159 * r * r,
        Shape.Rect(w, h) => w * h,
        Shape.Empty      => 0.0,
    }
}

fn largest[T](xs: [T]) -> ?T {       // generics, monomorphised
    if len(xs) == 0 {
        return nil                   // `?T` is an optional
    }
    let mut best = xs[0]
    for x in xs {
        if x > best {
            best = x
        }
    }
    return best
}

fn load(path: Str) -> !Str {         // `!T` is a value or an Error
    let text = fs.read_file(path)?   // `?` propagates the error
    if len(text) == 0 {
        return err("{path} is empty")
    }
    return ok(text)
}

fn main() -> !Void {
    let users = [User{ name: "ada", age: 36 }, User{ name: "alan", age: 41 }]
    let names = users.map(|u| u.name)            // closures
    io.println("{names.join(", ")} (max {MAX})")

    let ages = users.map(|u| u.age)
    io.println("oldest: {largest(ages) ?? 0}")   // `??` supplies a fallback

    let text = load("README.md")?
    io.println("README is {len(text)} bytes")
    return ok(void)
}
```

---

## Documentation

| document | what it covers |
|----------|----------------|
| [Getting started](docs/getting-started.md) | install, first project, the CLI |
| [Language tour](docs/tour.md) | every feature with examples |
| [Specification](spec/SPEC.md) | the authoritative reference |
| [Standard library](docs/stdlib.md) | every public API |
| [Memory model](docs/memory.md) | allocator, collector, lifetimes |
| [Compiler architecture](docs/compiler.md) | how velac works, stage by stage |
| [Bootstrap & self-hosting](docs/bootstrap.md) | build stages and what is self-hosted |
| [Diagnostics](docs/diagnostics.md) | how error messages are built |
| [Editor support](editor/README.md) | syntax highlighting and the language server |
| [Benchmarks](docs/performance.md) | measured compiler and runtime numbers |
| [Contributing](docs/contributing.md) | project layout and how to change it |

---

## Toolchain

```console
vela new <name>        create a project
vela init              create a project in the current directory
vela build             compile to build/<name>
vela run [args...]     build and run
vela check             type-check only
vela test              compile and run every `test` block
vela fmt [--check]     format to the canonical style
vela doc               generate Markdown API docs
vela add <n> --path P  add a local dependency
vela remove <name>     remove a dependency
vela clean             delete build/
vela repl              evaluate expressions interactively
```

The compiler can also be driven directly:

```console
velac -o prog main.vela      compile
velac --check main.vela      type-check
velac --emit-ir main.vela    print the optimised IR
velac --emit-tokens f.vela   print the token stream
velac --time main.vela       per-phase timings
```

---

## Examples

| file | shows |
|------|-------|
| [`examples/hello.vela`](examples/hello.vela) | the smallest program |
| [`examples/calculator.vela`](examples/calculator.vela) | a recursive-descent parser, enums, errors |
| [`examples/wordcount.vela`](examples/wordcount.vela) | files, maps, sorting, closures |
| [`examples/http_server.vela`](examples/http_server.vela) | TCP sockets and HTTP over raw syscalls |
| [`examples/life.vela`](examples/life.vela) | a simulation with nested lists and terminal output |
| [`examples/todo/`](examples/todo/) | a multi-file application with a local dependency |

---

## Testing

```console
$ ./tests/run.sh          # everything
$ ./tests/run.sh run      # golden-output programs
$ ./tests/run.sh fail     # diagnostics
$ ./tests/run.sh unit     # `test` blocks
$ ./tests/run.sh fmt      # formatter idempotency
$ ./tests/run.sh cli      # end-to-end toolchain
$ ./tests/run.sh fuzz     # randomised robustness
```

---

## Status

Vela 1.0 is complete and self-consistent for single-threaded programs:
the language, the compiler, the runtime, the standard library, the toolchain,
the formatter, the package manager and the documentation all agree.

Known limits, stated plainly:

* **x86-64 Linux only.** The backend emits ELF64 for one architecture.
* **Single-threaded.** `std/process` gives parallelism through child processes.
* **The compiler is not yet self-hosted.** The lexer, formatter, documentation
  generator and the `vela` driver are written in Vela; the type checker, IR and
  backend are still the C bootstrap. [`docs/bootstrap.md`](docs/bootstrap.md)
  describes exactly what is and is not self-hosted, and what remains.

## Licence

MIT. See [LICENSE](LICENSE).
