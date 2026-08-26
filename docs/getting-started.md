# Getting started

## Install a release

The quickest way in — no compiler needed:

```console
$ curl -fsSL https://github.com/NT9712/vela/releases/latest/download/vela-1.0.0-linux-x86_64.tar.gz | tar xz
$ cd vela-1.0.0-linux-x86_64
$ ./install.sh ~/.local
$ export PATH="$HOME/.local/bin:$PATH"
$ vela version
vela 1.0.0
```

`velac` finds its standard library next to itself, so no environment variable is
needed unless you move the binaries apart from `lib/vela`.

To build from source instead, read on.

## Requirements

* Linux on x86-64
* a C compiler (`cc` or `gcc`) and `make` — needed once, to build the bootstrap
  compiler

Nothing else. The compiler has no library dependencies and the programs it
produces have none either.

## Build the toolchain

```console
$ cd vela
$ make
cc -O2 ... -o bin/velac ...
VELA_ROOT=... bin/velac -o bin/vela tools/cli.vela
```

`make` builds two things:

* `bin/velac` — the bootstrap compiler, written in C
* `bin/vela` — the toolchain driver, **written in Vela** and compiled by `velac`

Put them on your `PATH` and tell the compiler where the standard library lives:

```console
$ export PATH="$PWD/bin:$PATH"
$ export VELA_ROOT="$PWD"
$ vela version
vela 1.0.0
```

`VELA_ROOT` is only needed if you move the binaries away from the checkout;
`velac` otherwise finds the library relative to its own location. It accepts
either the installation root (containing `lib/`) or the library directory itself
(containing `core/` and `std/`).

## Your first project

```console
$ vela new hello
created hello/

    cd hello
    vela run

$ cd hello
$ ls
.gitignore  src/  vela.toml
```

`vela.toml` is the manifest:

```toml
[package]
name    = "hello"
version = "0.1.0"
main    = "src/main.vela"

[deps]
```

`src/main.vela`:

```vela
use std/io

fn main() {
    io.println("hello from hello")
}

test "greeting is not empty" {
    assert(len("hello") > 0)
}
```

## Build, run, test

```console
$ vela run
hello from hello

$ vela build
built /home/you/hello/build/hello

$ ./build/hello
hello from hello

$ vela test
test greeting is not empty ... ok

1 passed, 0 failed, 1 total
```

The binary is static and self-contained:

```console
$ ls -l build/hello
-rwxr-xr-x 1 you you 10K build/hello
$ ldd build/hello
	not a dynamic executable
```

## Write something real

Replace `src/main.vela`:

```vela
use std/io
use std/os

struct Task {
    title: Str,
    done:  Bool,
}

fn Task.to_str(self) -> Str {
    let box = if self.done { "[x]" } else { "[ ]" }
    return "{box} {self.title}"
}

fn main() {
    let tasks = [
        Task{ title: "write a language", done: true },
        Task{ title: "write the docs", done: false },
    ]
    for i, t in tasks {
        io.println("{i + 1}. {t}")
    }
    let left = tasks.filter(|t| not t.done)
    io.println("{len(left)} remaining")
}
```

```console
$ vela run
1. [x] write a language
2. [ ] write the docs
1 remaining
```

Note `Task.to_str` — defining it changes how `Task` renders everywhere,
including inside lists and maps.

## Break it on purpose

```vela
fn main() {
    let name = "bob"
    let age = 30
    println(name + age)
}
```

```console
$ vela check
error: cannot add `Str` and `Int`
  --> src/main.vela:4:13
   |
 4 |     println(name + age)
   |             ^^^^^^^^^^
   = note: expected `Str + Str`
   = note: found    `Str + Int`
   = note: help: convert the right side with `str(x)`, or write `"{a}{b}"`

1 error
```

Try a few more: misspell a method, forget a `match` arm, assign to a `let` that
is not `mut`. The compiler is meant to tell you what to do next, not just what
is wrong.

## Format

```console
$ vela fmt
formatted src/main.vela

$ vela fmt --check      # exits 1 if anything would change; good for CI
1 file(s) already formatted
```

There is one canonical style and no options.

## Add a dependency

Dependencies are local directories with their own `vela.toml`.

```console
$ vela add mathx --path ../mathx
added mathx = ../mathx
```

```toml
[deps]
mathx = { path = "../mathx" }
```

```vela
use mathx/mathx

fn main() {
    println("{mathx.triple(14)}")
}
```

`vela add` also writes `vela.lock`, which records each dependency's resolved
path, version and a content hash, so a build is reproducible and you can see when
a local dependency changed.

## Generate documentation

```console
$ vela doc
wrote 1 page(s) to /home/you/hello/docs/api
```

`///` comments become Markdown, one page per module.

## The REPL

```console
$ vela repl
vela 1.0.0 repl - type an expression, or :help
> 1 + 2
3
> let xs = [3, 1, 2]
> xs.sort_by(|a, b| a < b)
> xs
[1, 2, 3]
> fn double(n: Int) -> Int { return n * 2 }
> double(21)
42
> :q
```

Definitions persist for the session. `:clear` forgets them, `:list` shows them.

## Where to go next

* [The tour](tour.md) — every feature, with examples
* [The specification](../spec/SPEC.md) — the precise rules
* [The standard library](stdlib.md) — every public API
* [`examples/`](../examples) — a calculator, a word counter, an HTTP server,
  Conway's life, and a multi-file application
