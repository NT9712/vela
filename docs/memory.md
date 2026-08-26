# The Vela memory model

Vela is garbage collected. There is no `free`, no ownership, no borrow checker
and no way to observe a dangling pointer from ordinary code. This document
explains what actually happens, because a garbage collector you cannot reason
about is a garbage collector you cannot trust.

Everything described here is implemented in [`lib/core/core.vela`](../lib/core/core.vela)
— in Vela, using the `@` intrinsics. You can read all of it.

---

## 1. Values

Every Vela value is exactly one 64-bit machine word.

| kind          | types                                              | the word holds |
|---------------|----------------------------------------------------|----------------|
| **primitive** | `Int`, `Float`, `Bool`, `Byte`, payload-free enums | the value      |
| **reference** | `Str`, `[T]`, `{K:V}`, structs, enums with payloads, closures, `?T`, `!T`, `Range` | the address of a heap object |

Assignment copies the word. For a primitive that copies the value; for a
reference it copies the reference, and both names then see the same object.

```vela
let a = Point{ x: 1.0, y: 2.0 }
let b = a          // b and a are the same object
b.x = 9.0
// a.x is now 9.0
```

That is the entire aliasing story. There is no hidden deep copy anywhere.

### Optionals

`?T` is `nil` (the word 0) or a `T`.

* If `T` is a reference type, `?T` costs nothing: `nil` is the null pointer.
* If `T` is a primitive, `Some(x)` allocates a 24-byte box so that `Some(0)` and
  `nil` stay distinguishable.

### Results

`!T` is always a 32-byte object: a tag word (0 = ok, 1 = error) and a payload
word. An error payload points at an `Error` object holding a message and a code.

---

## 2. Object layout

Every heap object starts with a 16-byte header:

```
 offset  size  field
 ------  ----  --------------------------------------------------------
   +0     u32  total size in bytes, including this header
   +4     u8   gc kind: 0 = atomic (contains no pointers), 1 = scan
   +5     u8   mark bit (used only during a collection)
   +6     u16  tag: the variant index of an enum, otherwise 0
   +8     u64  aux: byte length for Str, element count for List and Map
  +16          payload
```

The concrete shapes:

```
Str      +16.. the bytes, NUL-terminated for convenience
List     +16 capacity, +24 pointer to the element block (a separate object)
Map      +16 capacity, +24 entries block, +32 key type descriptor, +40 slots used
Result   +16 tag, +24 payload
Box      +16 the boxed primitive
Closure  +16 code address, +24 capture count, +32.. captured cells
Struct   +16 + 8*i  field i
Enum     +16 + 8*i  payload i  (variant index lives in the header tag)
```

Lists keep their elements in a separate block so that growing a list does not
have to move the list object other names may be pointing at.

---

## 3. Allocation

The heap is one 1 GiB `mmap` reservation, carved into 64 KiB **chunks**. Linux
only commits a page when it is first touched, so the reservation costs nothing
until it is used.

Each chunk serves a single **size class**. There are 21 classes, spaced 16 bytes
apart up to 128, then 32, then 128, then 512, up to 3328 bytes. Anything larger
gets a dedicated run of whole chunks.

A chunk's 2 KiB header holds a magic number, its size class, the object size,
the object count, and two bitmaps: which slots are allocated, and which are
marked.

Allocation is:

1. compute the size class,
2. pop the head of that class's free list,
3. zero the object, write the header, set its allocated bit.

That is a handful of instructions, with no search and no locking. When a free
list is empty a fresh chunk is carved and all of its slots are threaded onto the
list at once.

**Large objects** (> 3328 bytes) take a run of consecutive chunks. Freed runs are
marked as reusable and are found by a linear scan of the chunk table, which is
short because the table only has one entry per 64 KiB.

---

## 4. Collection

A **non-moving mark–sweep** collector, triggered only from `alloc`, only when
more than `threshold` bytes have been allocated since the last collection. The
threshold starts at 4 MiB and is then kept at twice the live size. A program
that does not allocate never collects.

Non-moving matters: addresses are stable for the whole life of an object, which
is what makes conservative roots safe and `@addr` meaningful.

### Roots

1. **Globals** — the module-level `const` table, scanned precisely (the compiler
   tells the runtime how many entries there are).
2. **The machine stack** — from the collector's own stack pointer up to the value
   of `rsp` recorded by `_start`, scanned word by word.
3. **Callee-saved registers** — `@save_regs()` pushes `rbx`, `r12`–`r15` onto the
   stack immediately before the scan, so they are covered by (2).

Stack scanning is **conservative**: any word that looks like the address of a
live object is treated as a root. This can retain a little garbage; it can never
free something reachable.

The compiler makes that soundness argument work:

* virtual registers are only ever allocated to callee-saved registers, so a
  value cannot be sitting in a caller-saved register across a call;
* all call arguments are evaluated into registers or frame slots *before* any of
  them are moved into argument registers, so a half-built argument list cannot be
  collected;
* everything that outlives a basic block lives in a frame slot.

### Tracing

Objects whose kind is `atomic` (strings, byte blocks, boxed primitives) are never
scanned. Objects whose kind is `scan` have every payload word treated as a
candidate pointer.

The grey set is an explicit mark stack of 1 Mi entries. If it overflows the
collector sets a flag, drains what it has, then rescans every marked object and
repeats until nothing new is found — slower, but bounded and correct.

### Sweeping

Sweeping walks the chunk table. For each allocated slot: if it is marked, clear
the mark and count it as live; otherwise clear its allocated bit and push it onto
its class's free list. Large runs that were not marked are returned to the pool.

Free lists are rebuilt from scratch on every sweep, which keeps them in address
order and improves locality.

### Observability

```vela
use core

core.gc_collections()   // collections so far
core.gc_live_bytes()    // reachable bytes after the last collection
core.gc_heap_bytes()    // address space handed out by the allocator
core.gc_collect()       // force a collection now
```

---

## 5. The stack

Locals live in the frame at `rbp - 8*(slot+1)`. A frame is laid out as:

```
 rbp
   -8*(1..nslots)          locals and compiler temporaries
   -8*(nslots+1..+nspill)  register-allocator spill slots
   -8*(...)                saved callee-saved registers
```

Frames are fixed size and everything is addressed from `rbp`, which is why
`@save_regs()` can safely push onto the stack mid-function.

### Captured variables

A local mentioned by a closure is **promoted to a heap cell** at compile time.
The enclosing function then reads and writes it through that cell, and the
closure captures the cell — so the closure and its parent see the same variable,
and the variable outlives the frame.

```vela
fn counter() -> fn() -> Int {
    let mut n = 0            // promoted: `n` lives in a heap cell
    return || { n += 1; n }  // the closure captures the cell
}
```

Variables that no closure mentions are never promoted and cost nothing.

---

## 6. Lifetime rules, stated plainly

* An object lives as long as any root can reach it, and is freed at some
  collection after that. There are no destructors and no finalisers.
* Object addresses never change.
* Reads and writes of a reference are ordinary loads and stores; there is no
  read or write barrier.
* Cycles are collected — a mark–sweep collector does not care about cycles.
* The only way to observe a freed object is to hold its address as a raw `Int`
  across an allocation. Only `@` intrinsic code can do that, which is why the
  intrinsics are documented as unsafe and confined to `lib/core`.

---

## 7. Concurrency

Vela 1.0 is single-threaded. The collector assumes one stack and no other mutator,
and the allocator has no locks.

For parallelism, `std/process` runs child processes and collects their output:

```vela
use std/process

let r = process.run(["./worker", "shard-1"])?
io.println(r.stdout)
```

A future version can add threads, but doing it honestly means adding safepoints,
per-thread allocation buffers and a stop-the-world protocol, so it is deliberately
out of scope for 1.0 rather than half-done.

---

## 8. Costs, measured

| operation                    | cost |
|------------------------------|------|
| allocate a small object      | free-list pop + header write |
| field read / write           | one load / store |
| collection                   | O(live) marking + O(heap) sweep |
| closure creation             | one allocation, plus one per captured variable |
| `?T` over a reference type   | free |
| `?T` over a primitive        | one 24-byte allocation |
| `!T`                         | one 32-byte allocation |
| string concatenation         | one allocation plus a copy |

See [performance.md](performance.md) for numbers from the benchmark suite.
