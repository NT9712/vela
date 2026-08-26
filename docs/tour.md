# The Vela tour

Everything in the language, with examples you can paste into `vela repl` or a
`main.vela`. For the precise rules see [the specification](../spec/SPEC.md).

---

## Hello

```vela
use std/io

fn main() {
    io.println("hello, world")
}
```

`println` and `print` are also in the prelude, so `println("hi")` works without
the import. `io` additionally gives you `eprintln`, `read_line` and `read_all`.

---

## Comments

```vela
// a line comment
/* a block comment
   /* which nests */ correctly */
/// a doc comment: attaches to the next declaration, read by `vela doc`
```

---

## Values and types

Four primitives, copied on assignment:

```vela
let n: Int    = 42            // 64-bit signed
let f: Float  = 3.14          // IEEE-754 double
let ok: Bool  = true
let c: Byte   = 'A'           // 0..255
```

Everything else is a reference to a garbage-collected object:

```vela
let s: Str          = "hello"
let xs: [Int]       = [1, 2, 3]
let m: {Str: Int}   = { "a": 1 }
let r: Range        = 0..10
```

Literals:

```vela
1_000_000    0xFF    0b1010    0o777       // Int
1.0    2.5e-3    1e9                       // Float
'a'    '\n'    '\x41'                      // Byte
"text"    "with {interpolation}"           // Str
```

There are **no implicit numeric conversions**. `1 + 1.0` is an error; write
`float(1) + 1.0`. This is on purpose: it is the single largest source of quiet
bugs in languages that allow it.

---

## Bindings

```vela
let x = 1                 // immutable, type inferred
let mut y = 2             // mutable
let z: Float = 3.0        // annotated
y = y + 1
y += 1                    // also -= *= /= %=
```

A `let` always initialises, so an uninitialised variable cannot exist.

---

## Strings

```vela
let name = "world"
println("hello, {name}!")            // interpolation
println("1 + 2 = {1 + 2}")           // any expression
println("a literal brace: {{ }}")    // {{ and }} escape

let s = "Hello, World"
s.len()                 // 12
s.upper()               // "HELLO, WORLD"
s.lower()               // "hello, world"
s.trim()
s[0..5]                 // "Hello"     slices are copies
s.split(", ")           // ["Hello", "World"]
s.words()               // splits on whitespace
s.lines()
s.replace("World", "Vela")
s.starts_with("He")     // true
s.contains("lo, W")     // true
s.index_of("World")     // 7
"ab".repeat(3)          // "ababab"
"42".to_int()           // ?Int
"3.5".to_float()        // ?Float
```

Strings are immutable and indexed by byte. `s[i]` yields a `Byte`.

---

## Lists

```vela
let xs = [1, 2, 3]
let empty: [Str] = []          // an empty literal needs a type

xs[0]                  // 1     panics if out of range
xs[1] = 9              // assign in place
len(xs)                // 3
xs[0..2]               // [1, 9]   a copy
xs + [4, 5]            // concatenation

xs.push(4)
xs.pop()               // 4
xs.insert(0, 0)
xs.remove(0)
xs.contains(9)
xs.index_of(9)         // -1 when absent
xs.first()             // ?T
xs.last()              // ?T
xs.reverse()
xs.copy()
xs.is_empty()

xs.map(|v| v * 2)
xs.filter(|v| v > 1)
xs.fold(0, |a, b| a + b)
xs.each(|v| println(str(v)))
xs.any(|v| v > 2)
xs.all(|v| v > 0)
xs.find(|v| v % 2 == 0)          // ?T
xs.sort_by(|a, b| a < b)         // in place, O(n log n), no allocation
xs.join(", ")
```

---

## Maps

```vela
let m = { "a": 1, "b": 2 }
let e: {Str: Int} = {:}          // empty

m["a"]                 // ?Int — lookup yields an optional
m["a"] ?? 0            // 1
m["z"] ?? 0            // 0
m["c"] = 3             // insert or update
m.has("a")
m.delete("a")
m.len()
m.keys()               // [Str], unspecified order
m.values()
m.get_or("z", -1)

for k, v in m {
    println("{k} = {v}")
}
```

Keys may be any type with structural equality: `Int`, `Str`, `Byte`, `Bool`,
`Float`, enums, structs and lists of those.

---

## Control flow

```vela
if x > 0 {
    println("positive")
} else if x == 0 {
    println("zero")
} else {
    println("negative")
}

while i < n {
    if skip { continue }
    if done { break }
    i += 1
}

for i in 0..n { }          // exclusive
for i in 0..=n { }         // inclusive
for x in xs { }
for i, x in xs { }         // index and value
for k, v in m { }
for b in "abc" { }         // bytes
```

Braces are always required.

### `if` as an expression

An `if` with an `else` produces a value, and a block's final expression is that
block's value:

```vela
let label = if n < 0 { "negative" } else if n == 0 { "zero" } else { "positive" }

let cost = if premium {
    let base = 100
    base * 2
} else {
    50
}
```

---

## Functions

```vela
fn add(a: Int, b: Int) -> Int {
    return a + b
}

fn greet(name: Str) {          // no `->` means it returns nothing
    println("hi, {name}")
}
```

Parameter and return types are always written out. Inside a function body
everything else is inferred.

### Methods

```vela
struct Point {
    x: Float,
    y: Float,
}

fn Point.length(self) -> Float {
    return (self.x * self.x + self.y * self.y).sqrt()
}

fn Point.scale(self, k: Float) {
    self.x = self.x * k
    self.y = self.y * k
}
```

Methods can be declared on any type in the same module and on the built-in types
— which is exactly how the standard library provides `Str.trim` and
`List[T].map`. There is no `impl` block and no inheritance.

### Closures

```vela
let double = |x| x * 2                    // types inferred from context
let add    = |a: Int, b: Int| a + b       // or written out
let sum    = |xs: [Int]| {                // a block body: last expression wins
    let mut t = 0
    for x in xs { t += x }
    t
}

fn make_counter() -> fn() -> Int {
    let mut n = 0
    return || { n += 1; n }               // captures `n` by reference
}
```

A captured variable is shared with its enclosing scope and outlives the frame.

---

## Structs

```vela
struct User {
    name: Str,
    age:  Int,
    tags: [Str],
}

let u = User{ name: "ada", age: 36, tags: [] }   // every field is required
u.age += 1
println("{u}")             // User{name: ada, age: 37, tags: []}
println("{u == u}")        // structural equality, recursive
```

Structs are references: assigning one does not copy it.

```vela
struct Pair[A, B] {
    first: A,
    second: B,
}

let p = Pair[Int, Str]{ first: 1, second: "one" }
```

---

## Enums

```vela
enum Color { Red, Green, Blue }              // no payloads: a plain integer

enum Shape {
    Circle(Float),
    Rect(Float, Float),
    Empty,
}

let c = Color.Green
let s = Shape.Rect(3.0, 4.0)
```

Generic enums work too:

```vela
enum Tree[T] {
    Leaf,
    Node(Tree[T], T, Tree[T]),
}
```

---

## Pattern matching

`match` is both a statement and an expression.

```vela
let area = match shape {
    Shape.Circle(r)  => 3.14159 * r * r,
    Shape.Rect(w, h) => w * h,
    Shape.Empty      => 0.0,
}

let size = match n {
    0            => "none",
    1 | 2 | 3    => "a few",
    _ if n < 100 => "some",
    _            => "many",
}

match point {
    Point{ x: 0, y: 0 } => println("origin"),
    Point{ x: a, y: b } => println("at {a},{b}"),
}

match maybe {
    nil     => println("missing"),
    some(v) => println("got {v}"),
}

match result {
    ok(v)  => println("ok {v}"),
    err(e) => println("failed: {e.msg}"),
}
```

Arms take an expression or a block; a block's final expression is the arm's
value. A `match` used as an expression must be exhaustive, and the compiler
tells you which cases you are missing.

---

## Optionals

```vela
let a: ?Int = nil
let b: ?Int = 5          // a plain value widens automatically

if let v = b {
    println("got {v}")
} else {
    println("nothing")
}

let c = b ?? 0           // fallback
let d = b?               // propagate nil out of a ?-returning function
```

`?T` and `T` are different types, so a missing value cannot be used by accident.

---

## Errors

Errors are values. `!T` is a `T` or an `Error { msg: Str, code: Int }`.

```vela
fn parse_port(s: Str) -> !Int {
    let n = s.to_int() ?? return err("not a number: {s}")
    if n < 1 or n > 65535 {
        return err("port out of range: {n}")
    }
    return ok(n)
}

fn main() -> !Void {
    let p = parse_port("8080")?          // propagates the error
    let q = parse_port("bad") ?? 80      // or supplies a default
    println("{p} {q}")

    match parse_port("x") {
        ok(v)  => println("ok {v}"),
        err(e) => println("error: {e.msg} (code {e.code})"),
    }
    return ok(void)
}
```

`main` may return nothing, an `Int` exit code, or `!Void` — in which case an
error is printed to stderr and the process exits non-zero.

There are no exceptions and no stack unwinding. `panic("...")` exists for
genuinely unrecoverable states; it prints and exits 101.

---

## Generics

```vela
fn largest[T](xs: [T]) -> ?T {
    if len(xs) == 0 {
        return nil
    }
    let mut best = xs[0]
    for x in xs {
        if x > best {
            best = x
        }
    }
    return best
}

largest([3, 1, 2])            // ?Int
largest(["b", "a"])           // ?Str
largest[Float]([1.5])         // explicit type argument
```

Generics are monomorphised and there are no trait bounds. If the body needs `>`
and the type has `>`, it compiles; if not, you get an error at the instantiation
site telling you which operation is missing and where the instantiation was.

---

## Modules

One file is one module. The last path segment is the binding name.

```vela
use std/io                 // -> io
use std/fs
use std/str as text        // rebind
use ./util                 // a sibling file util.vela
use ./sub/thing            // sub/thing.vela
use json/parse             // module `parse` of the dependency `json`
```

`pub` exports a declaration; without it, it is private to its module.

```vela
pub const VERSION: Str = "1.0"
pub struct Config { path: Str }
pub fn load() -> !Config { ... }
fn helper() { }            // private
```

---

## Tests

```vela
test "addition works" {
    assert(1 + 1 == 2)
    assert_eq(2 + 2, 4)
    assert_ne("a", "b")
}

test "errors propagate" {
    let v = might_fail()?          // `?` works: a test body is a `!Void`
    assert_eq(v, 42)
}
```

`vela test` compiles every `test` block in the project into one binary and runs
it. A failed assertion or a propagated error fails that test.

---

## Type aliases

```vela
type Grid    = [[Int]]
type Handler = fn(Str) -> !Str
```

Aliases are transparent — the same type, under another name.

---

## Intrinsics

For the standard library only. These are how `core` talks to the machine.

```vela
@syscall(n, a1..a6)     @load8/16/32/64(addr)     @store8/16/32/64(addr, v)
@addr(x)                @ref[T](addr)             @sizeof[T]()
@f2bits(f)              @bits2f(i)                @fsqrt(f)
@stack_top()            @rt_base()                @argc() @argv() @envp()
@save_regs()            @restore_regs()           @trap()
```

If you find yourself reaching for one of these in application code, the standard
library is probably missing something.
