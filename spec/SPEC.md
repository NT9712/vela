# The Vela Language Specification

Version 1.0 · Authoritative reference for the Vela programming language.

> This document defines the language. When the implementation and this document
> disagree, one of them is a bug. See `docs/compiler.md` for how the
> implementation maps onto this specification.

---

## 0. Identity

Vela is a **small, statically typed, natively compiled application language**.

Design axes, in priority order:

1. **Simple** — one obvious way to do things. The whole language fits in this document.
2. **Fast** — compiles straight to x86-64 machine code. No VM, no interpreter, no
   JIT warm-up, no libc, no dynamic linker. A Vela binary is a static ELF that
   talks to the kernel directly.
3. **Expressive** — closures, generics, sum types, pattern matching, string
   interpolation, first-class errors.
4. **Cohesive** — the standard library is written in Vela using the same
   features you get. There is no privileged "builtin" layer you cannot read.

Non-goals: manual memory management, a trait/typeclass system, operator
overloading, inheritance, macros, implicit conversions, exceptions,
threads (v1.0).

---

## 1. Lexical structure

### 1.1 Source encoding

Source files are UTF-8. The extension is `.vela`. A file is a **module**.

### 1.2 Comments

```vela
// line comment, runs to end of line
/* block comment,
   /* nests */ correctly */
/// doc comment (attaches to the next declaration)
```

### 1.3 Identifiers

```
ident := (letter | '_') (letter | digit | '_')*
```
`letter` is an ASCII letter. Identifiers are case-sensitive.

### 1.4 Keywords

```
and    as     break  const  continue  else   enum   false  fn     for
if     in     let    match  mut       nil    not    or     pub    return
struct test   true   type   use       while
```

`self` is a contextual keyword valid only as the first parameter of a method.

### 1.5 Literals

| Kind    | Examples                              | Type    |
|---------|---------------------------------------|---------|
| Integer | `0` `42` `1_000_000` `0xFF` `0b1010` `0o777` | `Int`   |
| Float   | `1.0` `3.14` `1e9` `2.5e-3`           | `Float` |
| Bool    | `true` `false`                        | `Bool`  |
| String  | `"hi"` `"a\nb"` `"x = {x}"`           | `Str`   |
| Char    | `'a'` `'\n'` `'\x41'`                 | `Byte`  |
| Nil     | `nil`                                 | `?T`    |

Integer literals are 64-bit signed. `_` may separate digits anywhere except at
the start. Escapes in strings and chars: `\n \r \t \\ \" \' \0 \xHH \{`.
Strings also accept `\u{H..H}`, which inserts the UTF-8 encoding of a Unicode
code point (1 to 6 hex digits).

**String interpolation.** Inside a `"..."` literal, `{expr}` splices the value of
`expr`, converted with `str(...)`. `\{` produces a literal `{`. A `}` outside an
interpolation is literal.

```vela
let name = "world"
io.println("hello, {name}! 1+2={1 + 2}")
```

### 1.6 Statement termination

Vela has **no semicolons**. Statements end at a newline. A statement continues
onto the next line when the line ends with a binary operator, a comma, or an
open bracket (`(`, `[`, `{`) that is not yet closed.

```vela
let total = a +
    b +
    c
```

---

## 2. Types

### 2.1 The type universe

```
Type := Int | Float | Bool | Byte | Str
      | List[T] | Map[K, V]
      | ?T                       // optional
      | !T                       // fallible: T or Error
      | fn(T1, ..., Tn) -> R     // function
      | Range
      | Name | Name[T1, ...]     // struct / enum, possibly generic
      | Void                     // the type of a function returning nothing
```

Sugar:

| Sugar     | Means        |
|-----------|--------------|
| `[T]`     | `List[T]`    |
| `{K: V}`  | `Map[K, V]`  |
| `?T`      | optional `T` |
| `!T`      | `T` or `Error` |

### 2.2 Value kinds

Vela has exactly two value kinds:

* **Primitive** — `Int` (64-bit signed), `Float` (IEEE-754 binary64), `Bool`,
  `Byte` (8-bit unsigned). Copied on assignment. Never `nil` unless wrapped in `?`.
* **Reference** — `Str`, `List`, `Map`, `Range`, structs, enums with payloads,
  functions/closures, `!T`. These name a garbage-collected heap object.
  Assignment copies the *reference*, not the object.

This is the single most important rule in the language:

> **Primitives are copied. Everything else is a reference to a GC object.**

There is no `&`, no `*`, no ownership, no borrow checker, and no way to observe
a dangling pointer from safe code.

### 2.3 Structs

```vela
struct Point {
    x: Float,
    y: Float,
}

pub struct User {
    name: Str,
    age:  Int,
    tags: [Str],
}
```

Construction uses the type name with named fields; every field must be given:

```vela
let p = Point{ x: 1.0, y: 2.0 }
```

Field access and assignment: `p.x`, `p.x = 3.0`. Structs are mutable references.
Two struct values are `==` when they are the same object or all fields are `==`
(structural equality, recursive).

Generic structs:

```vela
struct Pair[A, B] { first: A, second: B }
let q = Pair[Int, Str]{ first: 1, second: "one" }
```

### 2.4 Enums

```vela
enum Shape {
    Circle(Float),
    Rect(Float, Float),
    Empty,
}

enum Color { Red, Green, Blue }     // no payloads: represented as an integer
```

Construction: `Shape.Circle(2.0)`, `Color.Red`. Enums are deconstructed with
`match` (§5.5). An enum whose variants all have zero payloads is a **primitive**
(unboxed integer); any other enum is a reference type.

Generic enums are allowed: `enum Tree[T] { Leaf, Node(Tree[T], T, Tree[T]) }`.

### 2.5 Optionals

`?T` is either `nil` or a `T`.

```vela
let a: ?Int = nil
let b: ?Int = 5          // implicit wrap
if let v = b {           // binds v: Int when b is not nil
    io.println("got {v}")
}
let c = b ?? 0           // default
let d = b?               // propagate nil out of a ?-returning fn
```

`?T` and `T` are distinct types. A `?T` cannot be used where `T` is expected
without unwrapping. A `T` is implicitly widened to `?T`.

### 2.6 Errors

`Error` is a built-in struct:

```vela
struct Error { msg: Str, code: Int }
```

`!T` is a value that is either `Ok(T)` or an `Error`. It is produced by
`ok(v)` / `err(msg)` / `err_code(msg, code)` and consumed by `?`, `??`, or
`match`.

```vela
fn parse_port(s: Str) -> !Int {
    let n = str.to_int(s) ?? return err("not a number: {s}")
    if n < 1 or n > 65535 {
        return err("port out of range: {n}")
    }
    return ok(n)
}

fn main() -> !Void {
    let p = parse_port("8080")?      // propagates the Error on failure
    io.println("port {p}")
    return ok(void)
}
```

`?` is the only propagation form. It is a postfix operator valid on `?T` (in a
function returning `?U` or `!U`) and on `!T` (in a function returning `!U`).
There are no exceptions and no stack unwinding.

`main` may be declared `fn main()`, `fn main() -> Int`, or `fn main() -> !Void`.
In the last form an `Error` result prints a diagnostic to stderr and exits 1.

### 2.7 Function types

```vela
let f: fn(Int, Int) -> Int = add
let g = |x: Int| x * 2
let h = |x, y| x + y            // param types inferred from context
xs.map(|x| x * x)
```

A closure captures the variables it mentions **by reference to their storage**;
captured variables are promoted to the heap so a closure may outlive its
defining frame.

### 2.8 Type inference

* `let` with an initialiser infers the type.
* Function parameters and return types are **always explicit** (except lambda
  parameters, which infer from the expected type).
* Struct/enum fields are always explicit.
* Empty collection literals need context: `let xs: [Int] = []`.
* Integer literals do not implicitly become `Float`. Write `2.0`, or `float(2)`.

### 2.9 Generics

Functions, structs and enums may be generic:

```vela
fn max[T](a: T, b: T) -> T {
    if a > b { return a }
    return b
}
```

Generics are **monomorphised**: each distinct instantiation is compiled
separately, and the body is type-checked *after* substitution. There are no
trait bounds; if `max[Str]` needs `>` on `Str` and `Str` has it, it compiles.
If it does not, you get an error at the instantiation site:

```
error: `>` is not defined for `Point`
  --> demo.vela:9:12
   |
 9 |     if a > b { return a }
   |        ^^^^^
   = note: in instantiation of `max[Point]`
  --> demo.vela:14:9
   |
14 |     let m = max(p1, p2)
   |             ^^^^^^^^^^^
```

Type arguments are inferred from the argument types when possible, otherwise
written explicitly: `max[Float](1.0, 2.0)`.

### 2.10 Type aliases

```vela
type Grid = [[Int]]
type Handler = fn(Str) -> !Str
```

Aliases are transparent: they are the same type as their definition.

---

## 3. Declarations

A module consists of declarations. Order does not matter; all top-level names
are visible to each other.

```vela
use std/io                 // import
use std/str as s           // aliased import

pub const MAX: Int = 100   // constant (compile-time evaluated)

pub struct P { x: Int }    // struct
pub enum E { A, B }        // enum
pub type T = [Int]         // alias

pub fn f(a: Int) -> Int {  // function
    return a + 1
}

fn P.norm(self) -> Int {   // method on P
    return self.x
}

test "f adds one" {        // test (only compiled by `vela test`)
    assert(f(1) == 2)
}
```

`pub` makes a declaration visible to other modules. Without it a declaration is
private to its module. Struct fields follow their struct's visibility.

### 3.1 Constants

`const` initialisers must be compile-time constant: literals, arithmetic on
constants, other constants, and string concatenation.

### 3.2 Methods

```vela
fn Point.length(self) -> Float { ... }        // read-only-by-convention
fn Point.scale(self, k: Float) { self.x = self.x * k; ... }
```

A method's first parameter is `self`, whose type is the receiver. Methods may be
declared on any type declared in the same module, and on the built-in types
`Str`, `List[T]`, `Map[K,V]`, `Int`, `Float`, `Byte`, `Bool`, `Range` (the
standard library does exactly this). Calls are `recv.method(args)`.

Method lookup is: (1) methods declared in the current module, (2) methods
declared in the module that declares the receiver type, (3) methods declared in
any `use`d module. Ambiguity is an error.

---

## 4. Modules

### 4.1 Paths

```vela
use std/io           // stdlib module, bound as `io`
use std/fs
use ./util           // sibling file util.vela, bound as `util`
use ./sub/thing      // sub/thing.vela
use json/parse       // module `parse` of package `json`
use std/io as term   // rebind
```

The last path segment is the binding name unless `as` is given. Members are
accessed with `.`: `io.println`, `fs.read_file`.

Resolution order for `use a/b/c`:

1. `a == "std"` → `<vela-root>/lib/std/b/c.vela` (or `.../b.vela` for `std/b`)
2. path starts with `./` or `../` → relative to the current file
3. otherwise → `deps/a/src/b/c.vela` (see §11 package manager)

Circular imports are an error and are reported with the full cycle.

### 4.2 The prelude

Every module implicitly has these in scope: the built-in types; `ok`, `err`,
`err_code`, `void`; `str`, `int`, `float`, `byte`, `bool` (conversions);
`len`, `assert`, `panic`, `print`, `println`; `Error`; `Option`-style `nil`.

---

## 5. Statements

```
stmt := let | assign | expr | return | if | while | for | match | break | continue | block
```

### 5.1 Bindings

```vela
let x = 1              // immutable
let mut y = 2          // mutable
let z: Float = 3.0     // annotated
y = y + 1              // assignment (only for `mut`)
y += 1                 // compound: += -= *= /= %=
```

Rebinding with a new `let` in the same scope shadows the old binding.
Reading an uninitialised variable is impossible: `let` always initialises.

### 5.2 If

```vela
if cond {
    ...
} else if other {
    ...
} else {
    ...
}

if let v = maybe { ... } else { ... }     // optional binding
```

Braces are mandatory. `if` is a statement, not an expression. Use `match` or a
ternary-free `if/else` with assignment where you would want an if-expression.

### 5.3 While / loop

```vela
while i < n {
    if skip { continue }
    if done { break }
    i += 1
}
```

### 5.4 For

```vela
for i in 0..n { }            // Range, exclusive upper bound
for i in 0..=n { }           // inclusive
for x in xs { }              // List[T] -> T
for k, v in m { }            // Map[K,V] -> K, V
for i, x in xs.enumerate() { }
```

### 5.5 Match

`match` is both a statement and an expression.

```vela
let name = match shape {
    Shape.Circle(r) if r > 10.0 => "big circle",
    Shape.Circle(_)             => "circle",
    Shape.Rect(w, h)            => "rect {w}x{h}",
    Shape.Empty                 => "empty",
}
```

Patterns:

```
pattern := '_'                      // wildcard
         | ident                    // binding
         | literal                  // Int / Float / Str / Bool / Byte / nil
         | Type '.' Variant         // enum, no payload
         | Type '.' Variant '(' pattern,* ')'
         | Type '{' field ':' pattern,* '}'   // struct
         | pattern '|' pattern      // alternation (no bindings)
```

A guard `if expr` may follow any pattern. Arms are tried top to bottom. A
`match` used as an expression must be exhaustive; the compiler proves
exhaustiveness for enums, `Bool`, optionals and `!T`, and otherwise requires a
`_` arm. Unreachable arms are a warning.

Match arms take either an expression (`=> expr,`) or a block (`=> { ... }`).

### 5.6 Blocks and scope

Every `{ ... }` introduces a scope. Names are visible from their `let` to the
end of the enclosing block.

---

## 6. Expressions

### 6.1 Operator precedence

From loosest to tightest:

| Level | Operators                          | Assoc |
|-------|------------------------------------|-------|
| 1     | `or`                               | left  |
| 2     | `and`                              | left  |
| 3     | `== != < <= > >=`                  | none  |
| 4     | `..` `..=`                         | none  |
| 5     | `\|` `^`                           | left  |
| 6     | `&`                                | left  |
| 7     | `<< >>`                            | left  |
| 8     | `+ -`                              | left  |
| 9     | `* / %`                            | left  |
| 10    | unary `- not ~`                    | right |
| 11    | `?` (postfix), `??`                | left  |
| 12    | call `f(x)`, index `a[i]`, field `a.b`, `as` | left |

`and` / `or` short-circuit. `not` is logical negation on `Bool`; `~` is bitwise
complement on `Int`.

`a ?? b` evaluates to `a` unwrapped if `a` is non-nil/ok, otherwise `b`. The
right side may be a `return`/`break`/`continue`.

### 6.2 Operator typing

| Operator            | Valid operand types                                  | Result |
|---------------------|------------------------------------------------------|--------|
| `+`                 | `Int,Int` `Float,Float` `Str,Str` `[T],[T]` `Byte,Byte` | same   |
| `- * / %`           | `Int,Int` `Float,Float` (`%` not on Float)           | same   |
| `& \| ^ << >> ~`    | `Int,Int` `Byte,Byte`                                | same   |
| `== !=`             | any two values of the same type                      | `Bool` |
| `< <= > >=`         | `Int` `Float` `Str` `Byte`                           | `Bool` |
| `and or not`        | `Bool`                                               | `Bool` |
| `..` `..=`          | `Int,Int`                                            | `Range`|

There are **no implicit numeric conversions**. `1 + 1.0` is an error; write
`float(1) + 1.0`.

Integer division by zero and `%` by zero panic. `/` on `Int` truncates toward
zero.

### 6.3 Conversions

```vela
int(3.9)        // 3       (truncates toward zero)
int("42")       // ?Int    (nil when malformed)
int('A')        // 65
float(3)        // 3.0
float("1.5")    // ?Float
str(42)         // "42"    (works for every type)
byte(65)        // 'A'     (wraps mod 256)
bool(0)         // false   (Int/Byte: != 0)
```

`x as T` is the checked cast form used for enum payload access in rare cases and
for `Int`↔`Byte`; it is otherwise unnecessary.

### 6.4 Collection literals

```vela
let xs = [1, 2, 3]                 // [Int]
let ys: [Str] = []
let m = { "a": 1, "b": 2 }         // {Str: Int}
let e: {Str: Int} = {:}            // empty map
```

Indexing:

```vela
xs[0]        // T          — panics if out of bounds
m["a"]       // ?V         — nil when absent
m["a"] = 3   // insert or update
xs[0] = 9    // assign, panics if out of bounds
xs[1..3]     // [T] slice (copy)
s[1..3]      // Str slice
```

### 6.5 String interpolation, `str`, and `Show`

`str(x)` produces a `Str` for any value:

* primitives → their literal form
* `Str` → itself
* `[T]` → `[a, b, c]`
* `{K:V}` → `{k: v}`
* struct → `Name{f: v, ...}`
* enum → `Name.Variant(payload)`
* `?T` → `nil` or the inner value
* `!T` → `ok(v)` or `error: msg`

If a type declares `fn T.to_str(self) -> Str`, that is used instead.

---

## 7. Memory model

See `docs/memory.md` for the full treatment. Summary:

* **Allocation.** Reference values are allocated from a size-classed
  segregated heap carved out of one large `mmap` reservation. Allocation is a
  free-list pop (a few instructions).
* **Reclamation.** A **non-moving mark–sweep garbage collector**. Roots are the
  machine stack, callee-saved registers, and the globals table. Stack scanning
  is *conservative* (any 8-byte word that looks like a heap object start is
  treated as a root); heap scanning is *precise about which regions to scan*
  (each object header names a scan shape) and conservative about the contents.
* **Non-moving** means addresses are stable, so conservative roots are safe and
  `@addr` is meaningful.
* **Determinism.** Collection happens only at allocation points. A program that
  does not allocate never collects.
* **Stack.** Locals live in the frame. Locals captured by a closure are
  promoted to a heap cell at compile time.
* **Concurrency.** Vela 1.0 is single-threaded. `std/process` provides
  parallelism via child processes.

---

## 8. Execution model

Compilation is **whole-program**:

```
main.vela + imports + stdlib
      -> lex -> parse -> resolve -> typecheck -> monomorphise
      -> Vela IR -> optimise -> x86-64 -> static ELF64
```

There is no runtime linker, no libc, and no interpreter. `_start` sets up the
heap, records the stack base for the GC, marshals `argc`/`argv`/`envp`, calls
`main`, and issues `exit_group`. All I/O is direct `syscall`.

A `panic` prints `panic: <message>` plus a source location to fd 2 and exits
with status 101.

---

## 9. Tests

```vela
test "addition" {
    assert(1 + 1 == 2)
    assert_eq(2 + 2, 4)
}
```

`test` blocks are compiled only by `vela test`, which links them into a test
binary with a harness. `assert`, `assert_eq`, `assert_ne` are in the prelude.

---

## 10. Grammar (EBNF)

```ebnf
module      = { decl } ;
decl        = [ "pub" ] ( use | const | struct | enum | alias | fn | test ) ;

use         = "use" path [ "as" ident ] ;
path        = ident { "/" ident } | ( "." | ".." ) "/" ident { "/" ident } ;

const       = "const" ident ":" type "=" expr ;
struct      = "struct" ident [ generics ] "{" { field "," } "}" ;
field       = ident ":" type ;
enum        = "enum" ident [ generics ] "{" { variant "," } "}" ;
variant     = ident [ "(" type { "," type } ")" ] ;
alias       = "type" ident "=" type ;
generics    = "[" ident { "," ident } "]" ;

fn          = "fn" [ ident "." ] ident [ generics ] "(" [ params ] ")"
              [ "->" type ] block ;
params      = ( "self" | param ) { "," param } ;
param       = ident ":" type ;
test        = "test" string block ;

type        = "?" type
            | "!" type
            | "[" type "]"
            | "{" type ":" type "}"
            | "fn" "(" [ type { "," type } ] ")" [ "->" type ]
            | ident [ "[" type { "," type } "]" ] ;

block       = "{" { stmt } "}" ;
stmt        = let | assign | if | while | for | match | return
            | "break" | "continue" | expr ;
let         = "let" [ "mut" ] ident [ ":" type ] "=" expr ;
assign      = lvalue ( "=" | "+=" | "-=" | "*=" | "/=" | "%=" ) expr ;
lvalue      = ident | postfix "." ident | postfix "[" expr "]" ;
if          = "if" ( expr | "let" ident "=" expr ) block
              [ "else" ( if | block ) ] ;
while       = "while" expr block ;
for         = "for" ident [ "," ident ] "in" expr block ;
return      = "return" [ expr ] ;
match       = "match" expr "{" { arm } "}" ;
arm         = pattern [ "if" expr ] "=>" ( expr | block ) [ "," ] ;

expr        = or_expr ;
(* see §6.1 for the full precedence chain *)
primary     = literal | ident | "(" expr ")" | list_lit | map_lit
            | lambda | struct_lit | match | intrinsic ;
lambda      = "|" [ lparams ] "|" ( expr | block )
            | "fn" "(" [ params ] ")" [ "->" type ] block ;
intrinsic   = "@" ident [ "[" type { "," type } "]" ] "(" [ args ] ")" ;
```

---

## 11. Packages

A package is a directory with `vela.toml`:

```toml
[package]
name    = "hello"
version = "0.1.0"
main    = "src/main.vela"

[deps]
json = { path = "../json" }
```

* `vela build` resolves dependencies, writes `vela.lock`, and compiles.
* Dependencies live in `deps/<name>` (a symlink or copy for path deps).
* A module in package `p` is imported as `use p/module`.
* Versions are `major.minor.patch`; a lockfile pins exact resolved paths and
  content hashes so builds are reproducible.

---

## 12. Reserved for future versions

`async`, `await`, `spawn`, `trait`, `impl`, `where`, `defer`, `unsafe`, `macro`,
`i8`..`u64`, `move`, `static`. These are not keywords in 1.0 but are reserved
identifiers the compiler warns about.

---

## 13. Intrinsics (unsafe, for the runtime only)

Available in any module, but only the standard library is expected to use them.
They are the primitive operations the runtime is built from.

```
@syscall(n, a1..a6) -> Int      raw Linux syscall
@load8/@load16/@load32/@load64(addr: Int) -> Int
@store8/@store16/@store32/@store64(addr: Int, v: Int)
@addr(x) -> Int                 address of a reference value
@ref[T](a: Int) -> T            reinterpret an address as a reference
@sizeof[T]() -> Int             size in bytes of T's representation
@stack_top() -> Int             stack base recorded by _start
@save_regs() -> Int             spill callee-saved registers, return rsp
@restore_regs()                 undo @save_regs
@f2bits(f: Float) -> Int        bit reinterpretation
@bits2f(i: Int) -> Float
@trap()                         emit ud2 (unreachable)
```

Misuse of an intrinsic is undefined behaviour. Everything above this layer is
safe.
