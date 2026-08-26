# The Vela compiler

`velac` is a whole-program, ahead-of-time compiler. It reads one `.vela` file,
follows its imports (including the standard library, which is itself Vela), and
writes a static ELF64 executable. There is no assembler, no linker, no libc and
no runtime loader in that path.

```
main.vela + imports + lib/core + lib/std
        │
        ▼
    ┌────────┐   tokens    ┌────────┐    AST     ┌───────────┐
    │ lexer  │ ──────────► │ parser │ ─────────► │ resolver  │
    └────────┘             └────────┘            └───────────┘
                                                       │ scopes, modules
                                                       ▼
                                              ┌──────────────────┐
                                              │  type checker    │
                                              │ + monomorphiser  │
                                              └──────────────────┘
                                                       │ typed AST, one
                                                       │ instance per generic
                                                       ▼
                                              ┌──────────────────┐
                                              │  IR generation   │
                                              └──────────────────┘
                                                       │ Vela IR
                                                       ▼
                                              ┌──────────────────┐
                                              │  verifier        │
                                              │  optimiser       │
                                              └──────────────────┘
                                                       │
                                                       ▼
                                              ┌──────────────────┐
                                              │ x86-64 selection │
                                              │ register alloc   │
                                              │ encoding         │
                                              └──────────────────┘
                                                       │
                                                       ▼
                                                 ELF64 executable
```

Source layout:

| file | lines | responsibility |
|------|-------|----------------|
| `bootstrap/src/util.c`   | ~330 | arena, vectors, buffers, interning, source files, diagnostics |
| `bootstrap/src/lex.c`    | ~470 | tokens, numeric bases, string interpolation, nested comments |
| `bootstrap/src/parse.c`  | ~1200 | recursive descent, error recovery, desugaring |
| `bootstrap/src/types.c`  | ~280 | the type universe, interning, scopes |
| `bootstrap/src/sema.c`   | ~2300 | name resolution, type checking, monomorphisation |
| `bootstrap/src/irgen.c`  | ~1900 | typed AST → Vela IR |
| `bootstrap/src/opt.c`    | ~430 | verifier, constant folding, DCE, reachability, IR dump |
| `bootstrap/src/rodata.c` | ~300 | string literals, static closures, type descriptors |
| `bootstrap/src/x64.c`    | ~1000 | instruction selection, register allocation, encoding, ELF |
| `bootstrap/src/main.c`   | ~400 | driver, module resolution, CLI |

---

## 1. Lexer

Newline-sensitive: Vela statements end at a line break, so `T_NEWLINE` is a real
token. Runs of blank lines are collapsed into one token that remembers how many
blank lines there were, which is what lets the formatter preserve paragraphs.

String interpolation is handled by re-entering the lexer: `"total: {a + b}"`
becomes a token holding a list of pieces, where each expression piece carries its
own token stream. `{{` and `}}` are literal braces, but only outside an
interpolation — inside one, `}}` can legitimately close a struct literal and then
the interpolation.

## 2. Parser

Plain recursive descent with a precedence-climbing expression parser. Two things
are worth calling out.

**Struct literals versus blocks.** `if x { ... }` and `let p = Point{ ... }` both
have `ident {`. The parser carries a `no_struct` flag that is set while parsing
the header of `if`, `while`, `for` and `match`, exactly where a `{` must open a
block.

**Desugaring.** Several constructs are lowered in the parser so that the rest of
the compiler never sees them:

| surface syntax | becomes |
|----------------|---------|
| `if c { a } else { b }` in expression position | `match c { true => a, false => b }` |
| a trailing `if`/`else` in a value block | the same `match` |
| `\|x\| expr` | a lambda whose body is `return expr` |
| a lambda block's last expression | `return` of that expression |

Error recovery synchronises at statement and declaration boundaries, so one bad
line produces one error rather than a cascade. Recursion depth and operand-chain
length are both bounded so that pathological input produces a diagnostic instead
of a stack overflow.

## 3. Types and name resolution

Types are interned, so `ty_eq` is usually a pointer comparison. Generic struct
and enum *instances* are interned by `(declaration, type arguments)`, which is
what makes `Pair[Int, Str]` a single type no matter how many times it is written.

Names resolve through a chain of scopes: block → function → module. Modules are
first-class scope entries, so `io.println` is a module member lookup rather than
a field access. Methods live in a separate table keyed by `(type key, method
name)`, which is what lets the standard library add `Str.trim` and `List[T].map`
without the compiler knowing about them.

## 4. Type checking and monomorphisation

Checking is bidirectional in a small way: `check_expr(e, want)` passes an
expected type down, which is what makes `let xs: [Int] = []`, `nil`, and lambda
parameter inference work.

Generic functions are **monomorphised**. Each distinct tuple of type arguments
clones the AST and checks it afresh, so:

* the body is checked against real types, and errors name real types;
* there are no trait bounds to write, and none to satisfy;
* diagnostics from inside a generic body carry a note pointing at the
  instantiation site.

Type arguments are inferred by unifying the *syntactic* parameter types against
the actual argument types. Lambda arguments are handled in a second pass, so
`xs.map(|x| x * 2)` can infer both the element type from the receiver and the
result type from the lambda body. Speculative checks run with diagnostics muted
and their results discarded, so a failed inference attempt never leaks an error.

Work is driven by a queue: every non-generic function is instantiated up front
(so `vela check` reports problems in code nothing calls yet), and generic
instances are appended as they are discovered.

## 5. Vela IR

A list of basic blocks holding three-address instructions over virtual
registers, with two invariants:

1. a virtual register is assigned exactly once, and
2. it is used only in the block that defines it.

Anything crossing a block boundary goes through a frame slot. That removes phi
nodes entirely and reduces register allocation to a per-block linear scan.

`ir_verify` checks both invariants on every build and refuses to emit code if
either is violated. It has caught real miscompilations — for example an operand
emitted after its consumer, which produced silently wrong code rather than a
crash.

Runtime operations are ordinary calls into `core`: `alloc`, `str_concat`,
`list_get`, `map_set`, `any_to_str`. The compiler knows their names, not their
implementations.

## 6. Type descriptors

`str(x)`, `==` on aggregates, and map hashing all need to walk a value's shape at
runtime. Rather than generating a stringify function per type, the compiler emits
a compact **type descriptor** into rodata:

```
+0  kind          Int/Float/.../Struct/Enum
+8  name          a static Str
+16 nsub          field or variant count
+24 subs          descriptors for fields, elements, or variant payloads
+32 names         field or variant names
+40 aux           enum primitivity, optional boxing
+48 to_str        a static closure, when the type defines `to_str`
```

One implementation of `any_to_str` in `core` then handles every type. If a type
declares `fn T.to_str(self) -> Str`, the compiler puts a pointer to it in the
descriptor, so custom formatting works even for a value nested inside a list
inside a map.

## 7. Optimisation

Deliberately modest, and all of it pays for itself on code produced by a
straightforward lowering:

* **constant folding** on integer and comparison operations;
* **branch folding** — a conditional branch on a known constant becomes a jump;
* **dead code elimination** — pure instructions whose result is unused;
* **unreachable block removal**;
* **whole-program reachability** — only functions reachable from `_start`,
  `$init`, the tests, and static closures embedded in rodata are emitted.

Constants that fold to a literal are inlined at every use, which also solves an
ordering problem: `core` needs syscall numbers before global initialisers have
run.

## 8. Code generation

**Registers.** Virtual registers are allocated only to callee-saved registers
(`rbx`, `r12`–`r15`); caller-saved registers are scratch inside a single
instruction sequence. This has three consequences: a call can never clobber a
live value, no spilling is needed around calls, and conservative stack scanning
is sound.

Float values live in frame slots and are loaded into `xmm0`/`xmm1` for each
operation. Integer and pointer code — which is most code — gets real registers.

**Calling convention.** Every function takes a hidden first argument: the
closure environment, or 0 for a direct call. Remaining arguments go in `rsi`,
`rdx`, `rcx`, `r8`, `r9`, then the stack. Results come back in `rax`; a `Float`
result comes back as its bit pattern. Function values are closure objects, so a
plain function used as a value is a static closure in rodata with no captures.

**Encoding.** `x64.c` writes instruction bytes itself: REX prefixes, ModRM, SIB
when the base is `rsp` or `r12`, displacement sizing, `rel32` fixups for calls
and jumps, and 10-byte `mov r64, imm64` forms that are patched with final
addresses.

**ELF.** Two `PT_LOAD` segments — text plus rodata as read-execute, and a
zero-filled read-write segment for globals and the runtime state area. The entry
point records `rsp` for the collector, marshals `argc`/`argv`/`envp`, calls
`core.rt_init`, the generated `$init`, then `main`, and finally `exit_group`.

A hello-world binary is about 10 KB and makes three `mmap` calls before `main`.

## 9. Diagnostics

Diagnostics are a first-class output, not an afterthought. Each carries a span, a
rendered source line with a caret, and any number of attached notes. The checker
attaches suggestions from edit distance over names in scope, and type errors show
`expected` and `found` on separate lines with a concrete fix.

```
error: cannot add `Str` and `Int`
  --> demo.vela:8:18
   |
 8 |     let result = name + age
   |                  ^^^^^^^^^^
   = note: expected `Str + Str`
   = note: found    `Str + Int`
   = note: help: convert the right side with `str(x)`, or write `"{a}{b}"`
```

Errors are de-duplicated by location and message, and capped at 25 by default so
a broken file does not scroll away the first real problem.

## 10. Invariants worth knowing

If you change the compiler, these are the things that will bite you:

1. **Emit operands before their consumers.** `ir_verify` will catch it, but only
   if you run it.
2. **Nothing crosses a block boundary in a virtual register.** If an expression
   can open blocks (`and`, `or`, `??`, `?`, `match`, map indexing), any value
   computed before it must be parked in a frame slot. `may_branch` and
   `gen_operands` exist for exactly this.
3. **Evaluate all call arguments before moving any into argument registers.**
   Otherwise a collection triggered by a later argument can free an earlier one.
4. **Every expression must be typed by sema.** `gen_expr` fails loudly rather
   than dereferencing a null type.
