# Diagnostics

A compiler that only says *no* wastes your time. Vela treats diagnostics as
output that has to be as good as the code it generates.

## Anatomy

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

Every diagnostic has:

* a **level** — `error`, `warning`, `note`, `help`
* a **headline** that names the problem in the user's vocabulary, not the
  compiler's
* a **location** and the source line, with the exact span underlined
* **notes** giving the concrete types involved and, wherever possible, the
  change that fixes it

## Principles

**Say what was expected and what was found, separately.** A single sentence that
mixes both is harder to scan than two aligned lines.

**Suggest a fix, not a lecture.** `help: convert with float(x)` beats a paragraph
about type theory.

**Point at the smallest span that is still meaningful.** The whole expression for
a type error, the operator for an operator error, the field name for an unknown
field.

**Guess at typos.** Unknown names, fields, methods and enum variants are matched
against what is in scope by edit distance:

```
error: `Str` has no method `lenght`
  --> demo.vela:4:15
   |
 4 |     println("{"abc".lenght()}")
   |               ^^^^^^^^^^^^^^
   = note: did you mean `.len()`?
```

**Explain the model when the model is the problem.** Optionals and results are
where newcomers get stuck, so those errors say what to do:

```
error: type mismatch in `let`
   = note: expected `Int`
   = note: found    `?Int`
   = note: help: unwrap with `x ?? default` or `if let v = x { ... }`
```

**Trace generic instantiations.** An error inside a generic body says both where
the body is and which instantiation caused it.

**Do not repeat yourself.** Identical errors at the same location are emitted
once, and output stops after 25 errors so the first real problem stays visible.

## Warnings

Warnings are for things that are legal but almost certainly not intended:

* unused variables and parameters (prefix with `_` to silence)
* a binding that shadows another in the same block
* an expression statement with no effect

`velac --werror` turns them into errors; `-q` suppresses them.

## Recovery

The parser synchronises at statement and declaration boundaries, so one bad line
gives one error rather than a cascade. The type checker keeps going after a
mismatch using the expected type, so a single wrong argument does not hide the
next ten problems.

## For contributors

Diagnostics live in `bootstrap/src/util.c` (rendering) and are raised with
`serr`/`swarn`/`diag_note` from `sema.c` and `parse.c`. When you add one:

1. name the problem in the user's terms;
2. attach the concrete types;
3. attach a fix if there is an obvious one;
4. add a case to `tests/fail/` asserting the substrings you promised.

`tests/fail/*.vela` files carry `// ERROR: <substring>` lines; the suite fails if
any of those substrings stops appearing, so a diagnostic cannot silently regress.
