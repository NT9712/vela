# Standard library reference

Everything here is written in Vela and lives under `lib/`. The prelude is always
in scope; everything else needs a `use`.

Generate an always-current version from the source with `vela doc`.

---

## Prelude — always in scope

### Conversions and inspection

| function | meaning |
|----------|---------|
| `str(x) -> Str` | text for any value; uses `T.to_str` when the type defines it |
| `int(x) -> Int` | from `Float` (truncates), `Byte`, `Bool` |
| `int(s: Str) -> ?Int` | parse, nil when malformed |
| `float(x) -> Float` | from `Int`, `Byte` |
| `float(s: Str) -> ?Float` | parse, nil when malformed |
| `byte(n: Int) -> Byte` | wraps modulo 256 |
| `bool(n: Int) -> Bool` | `n != 0` |
| `len(x) -> Int` | bytes in a `Str`, elements in a list or map |

### Output and failure

| function | meaning |
|----------|---------|
| `print(x...)`, `println(x...)` | write to standard output |
| `panic(msg: Str)` | print to stderr and exit 101; does not return |
| `assert(c: Bool, msg: Str = ...)` | fail unless `c` |
| `assert_eq(a, b)`, `assert_ne(a, b)` | fail with both values shown |
| `ok(v)`, `err(msg)`, `err_code(msg, code)` | build a `!T` |
| `void` | the value of type `Void` |

### `Str`

`len` `is_empty` `upper` `lower` `trim` `starts_with` `ends_with` `contains`
`index_of` `split` `words` `lines` `replace` `repeat` `at` `bytes` `to_int`
`to_float` `hash`

Indexing yields a `Byte`; slicing yields a copy. Strings are immutable.

### `List[T]`

`len` `is_empty` `push` `pop` `first` `last` `insert` `remove` `clear` `copy`
`reverse` `map` `filter` `fold` `each` `any` `all` `find` `index_of` `contains`
`sort_by` `join`

`sort_by` is an in-place heapsort: O(n log n) always, and it never allocates.

### `Map[K, V]`

`len` `is_empty` `has` `delete` `clear` `keys` `values` `get_or`

`m[k]` yields `?V`; `m[k] = v` inserts or updates. Iteration order is
unspecified. Keys may be any type with structural equality.

### `Int`, `Float`, `Byte`, `Range`

```
Int:    abs min max clamp hex
Float:  abs floor ceil round min max sqrt pow is_nan
Byte:   is_digit is_alpha is_alnum is_space str
Range:  len list
```

`Float.sqrt` compiles to a single `sqrtsd`.

---

## `std/io`

```vela
io.print(s)              io.println(s)
io.eprint(s)             io.eprintln(s)
io.read_line() -> ?Str   io.read_all() -> Str    io.read_exact(n) -> Str
io.write(fd, s) -> Int   io.read(fd, n) -> Str
io.STDIN io.STDOUT io.STDERR
```

Standard input is buffered; output is not, so a `println` is exactly one
`write`.

## `std/fs`

```vela
fs.read_file(path)  -> !Str          fs.write_file(path, text)  -> !Void
fs.read_lines(path) -> ![Str]        fs.append_file(path, text) -> !Void
fs.exists(path) -> Bool              fs.size(path) -> !Int
fs.is_dir(path) -> Bool              fs.is_file(path) -> Bool
fs.remove(path) -> !Void             fs.rename(from, to) -> !Void
fs.mkdir(path) -> !Void              fs.mkdir_all(path) -> !Void
fs.rmdir(path) -> !Void              fs.read_dir(path) -> ![Str]

fs.open(path) -> !File               fs.create(path) -> !File
fs.append(path) -> !File
File.read(n) -> Str    File.read_all() -> Str
File.write(s) -> !Int  File.close()
```

Errors carry the path and a readable reason (`no such file or directory`,
`permission denied`, ...).

## `std/os`

```vela
os.args() -> [Str]              os.env() -> {Str: Str}
os.getenv(name) -> ?Str         os.exit(code)
os.cwd() -> Str                 os.chdir(path) -> !Void
os.pid() -> Int                 os.sleep_ms(ms)
os.exe_path() -> Str
```

## `std/path`

```vela
path.join([parts]) -> Str      path.dir(p) -> Str      path.base(p) -> Str
path.ext(p) -> Str             path.stem(p) -> Str
path.is_absolute(p) -> Bool    path.normalize(p) -> Str
```

Purely textual: nothing here touches the filesystem.

## `std/math`

```vela
math.PI math.E math.TAU math.INF math.NAN
math.abs  math.sqrt math.floor math.ceil math.round math.min math.max
math.sin  math.cos  math.tan
math.log  math.log2 math.log10 math.exp  math.pow
math.atan math.atan2 math.hypot
```

The elementary functions use argument reduction plus series evaluation, and are
accurate to about 1e-12 over their usual ranges. `sqrt` is exact.

## `std/rand`

```vela
rand.seed(n)          rand.seed_from_clock()
rand.next() -> Int    rand.int_below(n) -> Int   rand.range(lo, hi) -> Int
rand.float01() -> Float                          rand.chance(p) -> Bool
rand.shuffle(xs)      rand.pick(xs) -> ?T
```

xoshiro256\*\*. The same seed always produces the same sequence.

## `std/time`

```vela
time.now_ns() time.now_ms() time.now_s()   time.mono_ns()
time.now() -> DateTime                     time.utc(secs) -> DateTime
time.is_leap(year) -> Bool                 time.duration_str(ns) -> Str
DateTime{ year, month, day, hour, minute, second }   DateTime.to_str()  // ISO 8601
```

Use `mono_ns` for elapsed time; it is unaffected by clock adjustments.

## `std/sort`

```vela
sort.ints(xs)   sort.floats(xs)   sort.strs(xs)   sort.by(xs, less)
sort.search_ints(xs, v) -> Int    sort.is_sorted(xs, less) -> Bool
```

## `std/json`

```vela
json.parse(text) -> !Json
Json.to_str() -> Str          Json.pretty() -> Str
Json.get(key) -> ?Json        Json.at(i) -> ?Json
Json.as_str() -> ?Str         Json.as_int() -> ?Int
Json.as_float() -> ?Float     Json.as_bool() -> ?Bool

enum Json { Null, Bool(Bool), Num(Float), Str(Str), List([Json]), Obj({Str: Json}) }
```

Parse errors name the byte offset. Object keys are sorted on output, so the same
document always serialises identically.

## `std/net`

```vela
net.listen(port) -> !Listener      Listener.accept() -> !Conn   Listener.close()
net.connect(host, port) -> !Conn
Conn.write(s) -> !Int              Conn.read(n) -> Str
Conn.read_all() -> Str             Conn.read_until(marker) -> Str
Conn.close()
net.parse_ip(s) -> ?Int
```

IPv4 TCP over raw syscalls. `host` must be a dotted quad — there is no resolver.

## `std/process`

```vela
process.run(args) -> !Output       // waits, captures stdout and stderr
process.spawn(args) -> !Int        // waits, inherits the parent's streams
process.which(name) -> ?Str        // PATH lookup
process.kill(pid, sig) -> !Void
Output{ status: Int, stdout: Str, stderr: Str }
```

## `std/fmt`

```vela
fmt.pad_left(s, w)  fmt.pad_right(s, w)  fmt.center(s, w)
fmt.zero_pad(n, w)  fmt.fixed(v, places) fmt.commas(n)  fmt.bytes(n)
fmt.table(rows) -> Str
```

## `std/testing`

```vela
assert_near(a, b, eps)   assert_contains(s, sub)   assert_has(xs, v)
assert_ok(r) -> T        assert_err(r)
```

`assert`, `assert_eq` and `assert_ne` are in the prelude. A test body is a
`!Void` function, so `?` works inside it and a propagated error fails the test.

## `core`

The runtime. Import it only when you need the collector's statistics or the
string builder.

```vela
core.gc_collections() -> Int    core.gc_live_bytes() -> Int
core.gc_heap_bytes()  -> Int    core.gc_collect()

core.buf() -> Buf               core.buf_with(cap) -> Buf
Buf.push(s)  Buf.push_byte(c)   Buf.len() -> Int  Buf.clear()  Buf.str() -> Str
```

`Buf` is what you want whenever a string is built in a loop: `+` copies, and
`n` concatenations of a growing string is O(n²).

Everything else in `core` — the allocator, the collector, the string and
collection primitives, the type-descriptor walker — is implementation and may
change.
