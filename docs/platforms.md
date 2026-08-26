# Platforms

Vela compiles to a native executable for five targets. There is no runtime to
install, no linker step, no libc, and no dynamic loader: the compiler writes a
finished binary and the operating system runs it.

| `--target`    | Output          | Executable format | System interface        |
| ------------- | --------------- | ----------------- | ----------------------- |
| `x86_64`      | Linux           | ELF64             | `syscall`               |
| `arm64`       | Linux           | ELF64             | `svc #0`                |
| `windows-x64` | Windows 10+     | PE32+             | kernel32 / ws2\_32      |
| `macos-x64`   | macOS on Intel  | Mach-O            | `syscall`, BSD class 2  |
| `macos-arm64` | Apple Silicon   | Mach-O, signed    | `svc #0x80`             |

`velac` cross-compiles to any of them from any of them. The target defaults to
the host, so on a Mac `velac hello.vela` produces a Mach-O and on Linux it
produces an ELF.

```
velac -o hello.exe --target windows-x64 hello.vela
velac -o hello     --target macos-arm64 hello.vela
```

## How the platforms differ

Nearly all of the difference is confined to one file. `use core/sys` resolves
to `lib/core/sys_<target>.vela`, and everything above it — the allocator, the
collector, the standard library, the toolchain — is written once against that
interface. Adding a platform means writing one file and teaching the backend to
emit the right container.

The interface is deliberately small but not minimal: it stops at the point
where the platforms genuinely diverge rather than at the raw system call. So it
contains `dir_read` and `spawn` rather than `getdents64` and `fork`, because
Windows has neither of the latter.

### Linux

The straightforward case. Syscalls by number, ELF with two program headers,
`_start` reads `argc`/`argv` off the stack.

### Windows

Windows has no stable system-call interface — the numbers change between
builds, and the supported boundary is the DLLs. So the Windows backend makes no
syscalls at all. `bootstrap/src/pe.c` holds a fixed table of the 46 kernel32
and ws2\_32 functions the runtime needs, and `sys_windows-x64.vela` calls them
through `@winapi(index, args...)`, which lowers to an indirect call through the
import address table.

That table is an ABI between the compiler and the standard library, so it is
append-only: adding a function is fine, reordering is not.

Three details that are easy to get wrong and cost real debugging time:

- The Microsoft x64 convention needs 32 bytes of shadow space and `rsp`
  16-byte aligned *at the call*, not after it.
- Functions returning `DWORD` leave the upper half of `rax` undefined, so
  `GetFileAttributesA` never compares equal to `-1` in 64 bits unless it is
  masked first.
- A `HANDLE` is 64 bits, so the descriptor pair from `pipe` cannot be two
  32-bit words. `sys.pipe` returns two 64-bit slots on every platform.

Windows also passes arguments and the environment as single strings rather than
vectors, so `os.args()` and `os.env()` split them; `os.args()` implements the
quoting rules `CommandLineToArgvW` documents, in both directions.

### macOS

macOS is BSD underneath, and three of its conventions differ from Linux:

- **Errors come back in the carry flag** with a positive `errno`, rather than
  as a negative return value. The backend normalises this immediately after
  the `syscall`, so the code above reads identically on every platform.
- **Some calls return two values.** `fork` reports in the second register
  whether this is the child; `pipe` returns both descriptors that way. There is
  nowhere in the language to put a second result, so the backend parks it in a
  fixed runtime slot that `core/sys` reads back.
- **x86-64 tags syscall numbers** with the BSD class, `0x2000000`. arm64 passes
  the bare number, in `x16` rather than `x8`, with `svc #0x80`.

The container is Mach-O with `LC_UNIXTHREAD` — no `dyld`, no libSystem. macOS
also wants a 4 GiB unmapped `__PAGEZERO` at address zero, so the image is based
at `0x1_0000_0000`, and arm64 requires 16 KiB segment alignment.

#### Code signing

arm64 macOS will not execute an unsigned binary. Not "warn" — the kernel kills
the process. So `velac` signs every Mach-O it writes with an ad-hoc signature:
an embedded `CodeDirectory` holding a SHA-256 hash of every 4 KiB page, which
is exactly what `codesign -s -` produces. The SHA-256 implementation is in
`bootstrap/src/macho.c`; adding a dependency on `codesign` would have meant the
toolchain only worked on a Mac.

`codesign --verify` accepts the result. So does the kernel.

## What is verified, and how

Honesty about testing matters more than a long list of platforms, so:

| Target        | Compiled | Structure checked | Executed                     |
| ------------- | -------- | ----------------- | ---------------------------- |
| `x86_64`      | yes      | yes               | yes, natively                |
| `arm64`       | yes      | yes               | yes, under qemu and in CI    |
| `windows-x64` | yes      | yes               | yes, under wine and on CI's Windows runners |
| `macos-x64`   | yes      | yes               | yes, on CI's Intel Mac runners |
| `macos-arm64` | yes      | yes               | yes, on CI's Apple Silicon runners |

"Structure checked" means an independent parser re-reads the emitted binary and
validates it: `tests/pecheck.py` for PE and `tests/machocheck.py` for Mach-O.
These are deliberately not written against the emitter's data structures — they
parse the file from the first byte, follow every offset, and confirm that the
section table, the import directory and the code signature are self-consistent.
`machocheck.py` recomputes every page hash in the `CodeDirectory`, which is the
same check macOS performs before it will run an arm64 binary.

The whole golden test suite and the whole standard-library suite run on every
platform, and the outputs must be byte-identical to Linux's.

## Building the toolchain on each platform

`velac` is a C program, so it builds anywhere with a C99 compiler:

```
make            # velac, vela, vela-lsp for the host
```

On macOS this produces Mach-O binaries directly. On Windows, build `velac` with
mingw or MSVC; the `vela` and `vela-lsp` tools are Vela programs and can be
cross-compiled from any host:

```
make dist OS=windows ARCH=x86_64
make dist OS=macos   ARCH=arm64
```

## Adding a platform

1. Write `lib/core/sys_<name>.vela` implementing the ~40 functions in the
   interface. `lib/core/sys_x86_64.vela` is the reference.
2. Add the target to `Target`, `target_name` and `target_from_name` in
   `bootstrap/src/elf.c`.
3. If the executable format is new, write a container emitter beside `elf.c`,
   `pe.c` and `macho.c`, and an independent structural checker in `tests/`.
4. If the system-call convention differs, handle it in the `IR_SYSCALL` case of
   the backend rather than in the language.
5. Add a section to `tests/run.sh` and a CI job that executes the result.

Nothing above `core/sys` should need to change. If it does, the interface is
drawn in the wrong place.
