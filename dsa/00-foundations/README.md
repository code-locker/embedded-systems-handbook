# 00 — Foundations: how C/C++ source becomes a running program

> Everything later (pointers, allocators, data structures) sits on this.
> If you know what the compiler, the linker and the loader each do, most
> "mysterious" bugs stop being mysterious.

---

## 1. Why this exists

You write `printf("hi")` in a `.c` file and a process prints `hi`. In between,
**four programs** touched your code (preprocessor, compiler, assembler,
linker) and a fifth (the dynamic loader) set it up in memory. Every class of
error lives in exactly one of these stages:

| Error message | Stage that produced it | Meaning |
|---|---|---|
| `fatal error: foo.h: No such file or directory` | preprocessor | include path wrong |
| `error: redefinition of 'struct point'` | compiler | header pasted twice (no guard) |
| `error: expected ';'` | compiler | syntax |
| `undefined reference to 'foo'` | **linker** | declared, never defined (or lib not given) |
| `multiple definition of 'foo'` | **linker** | defined in two `.o` files (function body in a header) |
| `error while loading shared libraries: libx.so` | **dynamic loader** | `.so` not found at run time |
| `Segmentation fault` | **run time** | your bug (see 01-pointers, 03-debugging) |

Knowing *which stage* is complaining tells you *where to look*. Then there is
the language itself: type sizes, integer promotions and undefined behaviour
decide whether code that compiles is actually correct.

---

## 2. Mental model

**Analogy — a factory line for a book.**
The *preprocessor* is the editor who pastes every quoted chapter (`#include`)
into the manuscript and replaces abbreviations (`#define`). The *compiler*
translates the manuscript into another language (assembly). The *assembler*
types it up into numbered pages with a list of "see page ??" references
(an object file with unresolved symbols). The *linker* binds all books into
one volume and fills in every "see page ??". The *loader* puts the volume on
the shelf (memory) and opens it at page 1 (`_start` → `main`).

```
  hello.c ──► [cpp] ──► hello.i ──► [cc1] ──► hello.s ──► [as] ──► hello.o ─┐
  (source)   preprocess  (pure C,     compile   (assembly)  assemble (machine  │
             #include,   no macros)                                 code +     │
             #define,                                             symbols)    │
             #if                                                             ▼
                                         libc.so / libm.a / other .o ──► [ld] ──► a.out
                                                                         link     (ELF executable)
                                                                                     │
                                                     run: kernel execve() + ld-linux.so
                                                                                     ▼
                                                                                  process
```

`gcc` is only a *driver*: it runs `cpp`/`cc1`, `as`, and `collect2`/`ld` for you.
`gcc -v hello.c` prints the exact commands it runs.

---

## 3. How it works

### 3.1 The compilation pipeline — see every stage

```bash
P=dsa/00-foundations/c/01_compilation_pipeline.c
gcc -E $P -o /tmp/p.i      # 1. preprocess only: macros expanded, headers pasted
gcc -S /tmp/p.i -o /tmp/p.s  # 2. compile to assembly (add -O2 and compare!)
gcc -c /tmp/p.s -o /tmp/p.o  # 3. assemble to a relocatable object (ELF, type REL)
gcc    /tmp/p.o -o /tmp/p    # 4. link: add crt1.o, libc, resolve symbols
gcc -save-temps -c $P        # or: keep all intermediates in the current dir
```

| Stage | Input → output | What it does | Look with |
|---|---|---|---|
| Preprocess | `.c` → `.i` | text only: `#include` paste, macro replace, `#if` remove, comments gone | `gcc -E`, `wc -l p.i` (thousands of lines from stdio.h!) |
| Compile | `.i` → `.s` | parse, type-check, optimise, emit asm for *one* translation unit | `gcc -S -O2 -fverbose-asm` |
| Assemble | `.s` → `.o` | encode instructions; record symbols and *relocations* (holes to patch) | `objdump -d -r p.o`, `nm p.o` |
| Link | `.o` + libs → exe | merge sections, place them at addresses, patch every relocation | `nm a.out`, `readelf -a` |

A **translation unit** = one `.c` file after preprocessing. The compiler sees
exactly one at a time; it knows nothing about your other files. That is why
you need declarations (headers) — and why only the linker can find
"undefined reference" errors.

The preprocessor is pure text substitution, which is why macros bite:

```
#define BAD_SQUARE(x)  x * x
BAD_SQUARE(1 + 2)   ──►   1 + 2 * 1 + 2   ==  5      (not 9!)
#define GOOD_SQUARE(x) ((x) * (x))
GOOD_SQUARE(i++)    ──►   ((i++) * (i++))  ==  UB    (still evaluates twice!)
```

Prefer `static inline` functions (C) or `constexpr` functions (C++) over
function-like macros whenever you can.

### 3.2 Object files and symbols (`nm`)

An object file is a set of **sections** plus a **symbol table**:

```
  hello.o (ELF relocatable)
  ┌──────────────┐
  │ .text        │ machine code of functions        nm letter: T / t
  │ .data        │ initialised globals/statics      D / d
  │ .bss         │ zero-initialised globals (size only, no bytes in the file)  B / b
  │ .rodata      │ string literals, const globals   R / r
  │ .symtab      │ name → (section, offset)
  │ .rela.text   │ "at offset 0x1c, insert address of printf"  (relocations)
  └──────────────┘
  U printf       ← undefined here: the linker must find it elsewhere
```

**Uppercase = global (external linkage), lowercase = local (`static`).**
Real output of `nm build/dsa/00-foundations/c/05_storage_and_qualifiers`:

```
0000000000005120 d counter.1       <- static local: lowercase, renamed so it cannot clash
0000000000005020 D g_boot_count    <- initialised global in .data
0000000000005060 d g_private       <- file-scope static
0000000000003020 R g_table         <- const global in .rodata
0000000000005e80 B g_zeroed        <- uninitialised global in .bss
0000000000002309 t helper          <- static function
```

Useful commands: `nm -C` (demangle C++), `nm -u` (only undefined),
`nm --size-sort -S` (biggest symbols — great for shrinking firmware),
`objdump -d --disassemble=main`, `readelf -S` (sections), `size a.out`
(text/data/bss totals).

### 3.3 Static vs dynamic linking

```
 STATIC (gcc -static)                      DYNAMIC (default)
 ┌──────────────────────────┐              ┌───────────────────┐   at run time
 │ your .text               │              │ your .text        │   ld-linux.so maps:
 │ + copies of printf,      │              │ PLT stubs ────────┼──►  libc.so.6
 │   malloc, ... from libc.a│              │ NEEDED: libc.so.6 │     (shared by every
 └──────────────────────────┘              └───────────────────┘      process, in RAM once)
  ~800 KB, runs anywhere,                   ~16 KB, needs the right .so on the
  no loader needed, but                     target, security fixes arrive with
  libc fixes need a rebuild                 a library update; ldd shows deps
```

* A **static library** `libfoo.a` is just an archive (`ar`) of `.o` files. The
  linker copies in only the objects that resolve some undefined symbol, and it
  scans **left to right**: `gcc main.o -lfoo` works, `gcc -lfoo main.o` can
  fail with "undefined reference".
* A **shared library** `libfoo.so` is built with `-fPIC -shared`. Calls go
  through the PLT/GOT and are resolved lazily (or at start with `-z now`).
* Where the loader searches: `DT_RUNPATH` in the binary, `LD_LIBRARY_PATH`,
  `/etc/ld.so.cache` (`ldconfig -p`), `/lib`, `/usr/lib`.

Try it with three tiny files:

```bash
mkdir -p /tmp/link && cd /tmp/link
printf 'int add(int a,int b){return a+b;}\n' > add.c
printf 'int add(int,int);\n#include <stdio.h>\nint main(void){printf("%%d\\n",add(2,3));}\n' > main.c
gcc -c add.c main.c && nm add.o main.o          # main.o: U add, U printf
gcc main.o -o broken                            # undefined reference to `add'
ar rcs libadd.a add.o && gcc main.o -L. -ladd -o st && ./st      # static lib
gcc -fPIC -shared add.c -o libadd.so && gcc main.o -L. -ladd -o dy
./dy                                            # error while loading shared libraries
LD_LIBRARY_PATH=. ./dy && ldd ./dy | grep add   # works; shows libadd.so
```

Embedded angle: initramfs tools and early-boot helpers are often linked
statically (no loader or libc in the rootfs yet). Cross-compiled userspace for
the BeagleBone is normally dynamic against the board's glibc, so the *sysroot*
glibc version must not be newer than the board's.

### 3.4 Types and sizes

The standard fixes only minimums. Linux on x86_64 **and** aarch64 is **LP64**:

| Type | Minimum | LP64 (x86_64, aarch64 Linux) | ILP32 (32-bit ARM, e.g. the TDA4VM R5F cores) |
|---|---|---|---|
| `char` | 8 bit | 8 (signed on x86, **unsigned on ARM**) | 8 |
| `short` | 16 | 16 | 16 |
| `int` | 16 | 32 | 32 |
| `long` | 32 | **64** | **32** |
| `long long` | 64 | 64 | 64 |
| pointer | — | 64 | 32 |
| `size_t` | — | 64 | 32 |

Rules: use `<stdint.h>` (`uint32_t`, `int16_t`) for registers, protocols and
file formats; `size_t` for sizes and indexes; `uintptr_t` when you must hold
an address in an integer; `ptrdiff_t` for pointer differences. Print them with
`%zu`, `%td`, and `PRIu32`/`PRIx64` from `<inttypes.h>`.

### 3.5 Integer promotion, conversion and overflow

Two rules explain almost every integer surprise:

1. **Integer promotion.** Before arithmetic, `char`, `short`, `uint8_t`,
   `uint16_t`, bit-fields — anything narrower than `int` — become `int`.
2. **Usual arithmetic conversions.** If the two operands still differ, the
   "bigger" type wins; if a **signed** and an **unsigned** of the same rank
   meet, the signed one becomes **unsigned**.

```
uint8_t a=200, b=100;   a + b      ──► int 300        (promoted, no wrap)
                        (uint8_t)(a+b) ──► 44          (wrap on store: 300 mod 256)
uint8_t m = 0x0F;       ~m         ──► int 0xFFFFFFF0  (promote, THEN invert!)
int -1  <  unsigned 1   ──► (unsigned)-1 = 4294967295 < 1 ──► FALSE
size_t 3 - size_t 5     ──► 18446744073709551614       (never negative)
```

| | unsigned overflow | signed overflow |
|---|---|---|
| Defined? | **Yes**: wraps modulo 2^N | **No**: undefined behaviour |
| Use | counters, hashes, timers (`jiffies`) | never rely on it; check first |
| Detect | compare before | `__builtin_add_overflow`, `-fsanitize=undefined`, `-ftrapv` |

The wrap is *useful*: `now - start` on `uint32_t` stays correct even when the
counter wraps between the two reads. That is how the kernel's `time_after()`
works.

### 3.6 Undefined behaviour

**UB = the standard imposes no requirement.** It is not "the CPU does
whatever it does"; the *optimiser* assumes UB never happens and reasons
backwards from that:

```c
int *p = get();
int v = *p;          // compiler: "p was dereferenced, so p != NULL"
if (p == NULL)       // ... so this check is dead code and is DELETED
    return -1;
```

That exact pattern caused a Linux kernel vulnerability (tun driver, 2009),
which is why the kernel builds with `-fno-delete-null-pointer-checks`.

The list to know: signed overflow, out-of-bounds access, NULL/dangling/
uninitialised pointer use, reading uninitialised locals, shifting by ≥ width
or shifting negatives left, strict-aliasing violations, data races, modifying
a string literal, modifying a variable twice between sequence points
(`i = i++`), division by zero and `INT_MIN / -1`, falling off the end of a
non-void function and using the value.

Also distinguish **implementation-defined** (must be documented: `char`
signedness, `>>` of negatives, `sizeof(long)`) and **unspecified** (one of
several choices, not documented: argument evaluation order).

### 3.7 `const`, `volatile`, `static`, `extern`

```
 keyword   at file scope                         inside a function
 ───────   ─────────────────────────────────     ──────────────────────────────────
 static    internal linkage: name is private     static storage: one copy, lives for
           to this .c (lowercase in nm)           the whole program, initialised once
 extern    "defined in another TU" — a           refers to a global defined elsewhere
           declaration, allocates nothing
 const     read-only object (.rodata in C);      read-only local; compiler error on
           in C++ also implies internal linkage   assignment
 volatile  every read/write must really          same — use for MMIO registers, flags
           happen, in program order               set by ISRs/signal handlers
```

* `volatile` does **not** make code thread-safe or atomic and adds no memory
  barriers. For threads use `_Atomic`/`std::atomic`; in the kernel use
  `READ_ONCE`/`WRITE_ONCE`, and `readl`/`writel` for MMIO (they include the
  barriers).
* Rule of thumb: every function and global that is not part of the module's
  API should be `static`. Smaller symbol table, better inlining, no clashes.
* `extern int x;` in a header + `int x = 0;` in exactly one `.c` = the correct
  way to share a global.

### 3.8 Header guards

`#include` is a paste. Two headers that both include `point.h` paste it twice
into the same translation unit → `redefinition of 'struct point'`.

```c
#ifndef POINT_H        // 1st paste: not defined → keep the body
#define POINT_H        //            now it is defined
struct point { int x, y; };
#endif                 // 2nd paste: #ifndef fails → body skipped
```

`#pragma once` does the same and is supported everywhere that matters. A
guard protects **one translation unit only**: a non-`static` function
*definition* in a header included from two `.c` files still gives
`multiple definition` at link time. Headers contain declarations, macros,
types, and `static inline` functions.

### 3.9 C vs C++ — the differences that matter

**References.** `int &r = x;` makes `r` another name for `x`. Must be
initialised, cannot be re-seated, cannot be null. Implemented as a pointer,
used like the object.

```
        x, r                     int *p = &x;          p           x
      ┌──────┐                                      ┌──────────┐  ┌──────┐
      │  12  │  one box, two names                  │0x7ffd..a4├─►│  12  │
      └──────┘  &r == &x                            └──────────┘  └──────┘
                                                    p is its OWN object (8 bytes)
```

**Overloading and name mangling.** C has one symbol per name. C++ encodes the
parameter types into the symbol, so `area(int)` and `area(double,double)` can
coexist:

```
area(int)            -> _Z4areai        _Z = mangled, 4 = name length,
area(double, double) -> _Z4areadd       then parameter types: i=int, d=double,
area(long)           -> _Z4areal        l=long, c=char, P=pointer, R=reference, K=const
```

**`extern "C"`** switches mangling off, so C code, `dlsym()` and assembly can
find the function. Every C header that might be used from C++ wraps itself:

```c
#ifdef __cplusplus
extern "C" {
#endif
int c_api_version(void);
#ifdef __cplusplus
}
#endif
```

Classic error when you forget it: C++ code calls `foo`, the linker looks for
`_Z3foov`, the C library exports `foo` → `undefined reference to 'foo()'`
(note the `()` — the tell-tale sign of a mangled C++ name).

**Other differences** (all demonstrated in `cpp/03_c_vs_cpp_differences.cpp`):
`'A'` is `int` in C but `char` in C++; `void*` does not convert implicitly to
`T*` in C++ (so `malloc` needs a cast — or better, do not use `malloc`);
`f()` means "unknown parameters" in C but "no parameters" in C++; `const int`
is a constant expression in C++ only; C++ has `nullptr`, `bool`, `constexpr`,
named casts; C has VLAs, `restrict`, designated initializers (C++ from 20).

---

## 4. Complexity table

This lesson is about tools, not algorithms, so the "cost" table is about
**when** each thing costs you:

| Thing | Cost paid at | Run-time cost |
|---|---|---|
| macro, `sizeof`, `_Static_assert`, `constexpr` | compile time | zero |
| `static inline` function | compile time (inlined) | usually zero call overhead |
| static linking | link time (bigger file) | direct calls, no symbol lookup |
| dynamic linking | load time (relocations) + first call (lazy PLT) | one indirect jump per call |
| `volatile` access | run time | one real memory access per use (no caching) |
| overflow check with `__builtin_add_overflow` | run time | 1 instruction + branch |

---

## 5. C vs C++ summary

| Concept | C | C++ |
|---|---|---|
| Constants | `#define`, `enum`, `const` (not a constant expr) | `constexpr`, `const` |
| Generic code | macros, `void*`, `_Generic` | templates, overloading |
| Pass by alias | pointer `T*` | reference `T&`, `const T&` |
| Null pointer | `NULL` (often `((void*)0)`) | `nullptr` |
| Casts | `(T)x` does everything | `static_cast`, `const_cast`, `reinterpret_cast`, `dynamic_cast` |
| Linkage control | `static` | `static`, anonymous `namespace {}` |
| Calling C from C++ | — | `extern "C"` |
| Compile-time checks | `_Static_assert` | `static_assert`, `<type_traits>` |

---

## 6. Where it is used in the real world

* **Linux kernel**: built with `-fno-strict-aliasing -fno-delete-null-pointer-checks
  -fwrapv`-like flags precisely because of UB; uses `check_add_overflow()`,
  `BUILD_BUG_ON()` (a static assert), `__iomem` + `readl()` instead of raw
  `volatile`, `EXPORT_SYMBOL` to decide what modules can link against.
* **U-Boot / TF-A on the TDA4VM**: linker scripts place `.text`/`.data`/`.bss`
  at fixed SRAM/DDR addresses; start-up assembly zeroes `.bss` and copies
  `.data` from flash — exactly the sections `nm` shows you.
* **GStreamer**: plugins are shared objects loaded with `dlopen`; each exports
  one `extern "C"`-style `gst_plugin_desc` symbol the registry looks up.
* **Yocto / Buildroot**: cross toolchains, sysroots, static vs dynamic decisions
  for initramfs; `-Wl,--gc-sections` to drop unused code.
* **Embedded firmware (R5F, MCUs)**: 32-bit ILP32 — `long` is 32 bits there,
  so code shared between A72 Linux and R5F firmware must use `<stdint.h>`.

---

## 7. Common mistakes and pitfalls

1. Function-like macros without parentheses, or with arguments that have side
   effects (`MAX(i++, j)`).
2. Putting a non-`static` function or variable definition in a header →
   `multiple definition` at link time.
3. Library order on the command line: `gcc -lm main.c` can fail; put libraries
   **after** the objects that use them.
4. Assuming `long` is 64 bits or `char` is signed (ARM: unsigned!).
5. `for (unsigned i = n - 1; i >= 0; i--)` — infinite loop. Use `for (size_t i = n; i-- > 0;)`.
6. `if (strlen(a) - strlen(b) < 0)` — never true (size_t).
7. Computing `w * h` in `int` and then storing into `long` — the overflow already happened.
8. `1 << 31` (UB on `int`) instead of `1u << 31` for register masks.
9. Thinking `volatile` makes a flag thread-safe.
10. Forgetting `extern "C"` when calling C from C++ (`undefined reference to 'foo()'`).
11. Ignoring warnings: `-Wall -Wextra` already catch most of the above.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_compilation_pipeline.c](c/01_compilation_pipeline.c) | macros, `#`/`##`, conditional compilation; header lists the `-E/-S/-c` commands | `make run P=dsa/00-foundations/c/01_compilation_pipeline.c` |
| [c/02_types_and_sizes.c](c/02_types_and_sizes.c) | sizes/ranges of every type, `<stdint.h>`, `PRIx32`, `_Static_assert` | `make run P=dsa/00-foundations/c/02_types_and_sizes.c` |
| [c/03_integer_promotion_overflow.c](c/03_integer_promotion_overflow.c) | promotion, signed/unsigned compare, wrap-around timers, safe add | `make run P=dsa/00-foundations/c/03_integer_promotion_overflow.c` |
| [c/04_undefined_behaviour.c](c/04_undefined_behaviour.c) | catalogue of UB with the safe alternative for each | `make run P=dsa/00-foundations/c/04_undefined_behaviour.c` |
| [c/05_storage_and_qualifiers.c](c/05_storage_and_qualifiers.c) | `static`, `extern`, `const`, `volatile`; which section each lands in | `make run P=dsa/00-foundations/c/05_storage_and_qualifiers.c` |
| [c/06_header_guards.c](c/06_header_guards.c) | a file that includes itself to prove a header guard works | `make run P=dsa/00-foundations/c/06_header_guards.c` |
| [c/07_linking_and_symbols.c](c/07_linking_and_symbols.c) | lists the shared objects the loader mapped; `nm`/`ldd`/static-build recipes | `make run P=dsa/00-foundations/c/07_linking_and_symbols.c` |
| [cpp/01_references.cpp](cpp/01_references.cpp) | references vs pointers, `const&`, returning references | `make run P=dsa/00-foundations/cpp/01_references.cpp` |
| [cpp/02_overloading_and_mangling.cpp](cpp/02_overloading_and_mangling.cpp) | overloading, mangled names, demangling, `extern "C"` | `make run P=dsa/00-foundations/cpp/02_overloading_and_mangling.cpp` |
| [cpp/03_c_vs_cpp_differences.cpp](cpp/03_c_vs_cpp_differences.cpp) | 11 places C and C++ disagree | `make run P=dsa/00-foundations/cpp/03_c_vs_cpp_differences.cpp` |
| [cpp/04_types_casts_constexpr.cpp](cpp/04_types_casts_constexpr.cpp) | `auto`, the four named casts, `constexpr` register masks | `make run P=dsa/00-foundations/cpp/04_types_casts_constexpr.cpp` |

---

## 9. Exercises

1. **(easy)** Run the four `gcc -E/-S/-c` commands on `01_compilation_pipeline.c`.
   How many lines is the `.i` file? Find `BAD_SQUARE` in it (you will not — why?).
2. **(easy)** Compile `02_types_and_sizes.c` with `-m32` (needs `gcc-multilib`)
   or with `aarch64-linux-gnu-gcc` and compare. Which rows change?
3. **(easy)** `nm` the `05_storage_and_qualifiers` binary. Remove `static` from
   `helper` and check the letter changes from `t` to `T`.
4. **(medium)** Do the `/tmp/link` experiment from §3.3. Then put `-ladd` *before*
   `main.o` and explain the error.
5. **(medium)** Write `int f(int x){ return x + 1 > x; }`, compile with
   `gcc -O2 -S` and read the assembly. Then add `-fwrapv`. *Hint:* the first
   version is `mov eax, 1`.
6. **(medium)** Remove the guard from `06_header_guards.c`; read the error. Now
   remove `static inline` from `point_manhattan` and split the file into two
   `.c` files that both include the header: which tool complains now?
7. **(hard)** Write a C library `libshape.so` with `double shape_area(double)` and
   call it from C++ without `extern "C"`. Read the linker error, then fix it in
   the header the right way.
8. **(hard)** Using `objdump -d -r` on a `.o`, find the relocation for the call
   to `printf`. Then link and find where it was patched to (the PLT entry).

---

## 10. Remember forever

```
cpp → cc1 → as → ld → ld-linux:   .c → .i → .s → .o → exe → process
gcc -E / -S / -c / (link);  -save-temps keeps them all;  nm: UPPER=global, lower=static, U=undefined
"undefined reference" = LINKER (missing definition or lib order); "redefinition" = COMPILER (no guard)
LP64: int 32, long 64, ptr 64.  ARM char is unsigned.  Use <stdint.h> for hardware.
Promotion: < int → int.  signed meets unsigned → unsigned (-1 < 1u is false).
unsigned overflow wraps (defined);  signed overflow is UB → check first.
static: file-private (file scope) / lives forever (block scope).  extern: declared, defined elsewhere.
volatile = every access happens; NOT atomic, NOT a barrier.
C++ mangles names (_Z4areai) for overloading; extern "C" turns that off.
```

---

## 11. Self-test questions

1. Which stage turns `#include <stdio.h>` into thousands of lines, and which
   stage first notices that `foo()` has no body anywhere?
2. What does `U` mean in `nm` output? What do `T` and `t` mean?
3. Why does `uint8_t a = 200, b = 100; int s = a + b;` give 300?
4. Why is `-1 < 1u` false, and how would you write that comparison correctly?
5. Why can the compiler delete `if (p == NULL)` after `*p` was read?
6. What is the difference between undefined, unspecified and
   implementation-defined behaviour? One example each.
7. `static int x;` at file scope vs inside a function — what does each mean?
8. Why does `volatile` not make a shared counter thread-safe?
9. A header guard is present but the linker says "multiple definition". Why?
10. What is name mangling, why does C++ need it, and what does `extern "C"` do?
11. Why is `sizeof(long)` different on the TDA4VM's A72 Linux and its R5F cores?

Review schedule: see [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).
