# Strings in depth — from bytes in `.rodata` to `std::string`'s SSO

> Part of [05 — Arrays and strings](../README.md). This is the in-depth strings
> lesson: where string bytes live, how libc implements them (and why it's
> fast), dynamic strings, how `std::string` works inside, UTF-8, and how
> strings turn into security bugs. Programs `c/10`–`c/18` and `cpp/10`–`cpp/13`
> go with it.

---

## 1. Why this exists

Almost every program reads, builds, compares, or prints text: device paths
(`/dev/video2`), sysfs values, GStreamer pipeline descriptions, log lines,
protocol messages, config files. Strings are also where a lot of C's worst bugs
come from: buffer overflows, missing terminators, format-string exploits, and
"it works for ASCII but not for `é`".

A C string is an **array of bytes ending with a `'\0'` byte**. Nothing more:
there is no length field and no capacity field, so nothing tracks who owns the
bytes. Every rule in this lesson follows from that one design choice (made in
1972 to save a byte per string). C++'s `std::string` and `std::string_view` exist
to repair it.

What you will be able to do afterwards:

- Say where any string's bytes live, whether you may write them, and how long they live.
- Write `strlen` … `strtok_r` from memory, including the corner cases.
- Explain the word-at-a-time bit trick behind a fast `strlen`.
- Build a growable string in C and explain SSO, copy vs move, and `c_str()` lifetime in C++.
- Encode, decode and validate UTF-8 by hand.
- Copy and format text without overflows, and know what a stack canary does.

---

## 2. Mental model

**A C string is a train of wagons with no sign saying how long it is. The last
wagon is empty (`'\0'`). To know the length, you walk the platform and count.**

```
 const char *p = "BBAI64";

 p ──► ┌────┬────┬────┬────┬────┬────┬────┐
       │ 'B'│ 'B'│ 'A'│ 'I'│ '6'│ '4'│ \0 │      7 bytes, length 6
       └────┴────┴────┴────┴────┴────┴────┘
        0x42 0x42 0x41 0x49 0x36 0x34 0x00
```

Two consequences you must never forget:

1. **Length is O(n).** `strlen` walks until it finds the zero. A loop such as
   `for (i = 0; i < strlen(s); i++)` therefore costs O(n²).
2. **No function knows the buffer size.** `strcpy(dst, src)` only knows where
   `src` ends. It does not know where `dst` ends.

The C++ model fixes both by storing the length (and capacity) next to the pointer:

```
 C string              std::string (libstdc++)          std::string_view
 ┌─────┐               ┌─────────┬──────┬──────────┐    ┌─────────┬──────┐
 │ ptr │──► b y t e s \0│ ptr     │ size │ cap/SSO  │    │ ptr     │ len  │
 └─────┘               └─────────┴──────┴──────────┘    └─────────┴──────┘
 length: walk, O(n)    owns its bytes, size O(1)       borrows bytes, O(1)
```

---

## 3. How it works

### 3.1 Where strings live in memory

Program: [`c/10_string_memory_layout.c`](../c/10_string_memory_layout.c)

```
 high addr ┌──────────────────────────┐
           │ stack            rw-     │  char s[] = "hi";      ← COPY of the literal
           │   ↓                      │
           │   ↑                      │
           │ heap             rw-     │  malloc(16), std::string's long buffer
           ├──────────────────────────┤
           │ .data / .bss     rw-     │  char g[] = "hello";   static char buf[64];
           ├──────────────────────────┤
           │ .rodata          r--     │  "hi"  "hello"  every string LITERAL
           ├──────────────────────────┤
           │ .text            r-x     │  code
 low addr  └──────────────────────────┘
```

| Declaration | Where the bytes are | Writable? | Lifetime |
|---|---|---|---|
| `"text"` (a literal) | `.rodata` | **No**, SIGSEGV | whole program |
| `const char *p = "text";` | `p` on stack, bytes in `.rodata` | No | program |
| `char s[] = "text";` (local) | stack: a **copy** | Yes | until `}` |
| `char g[] = "text";` (global/static) | `.data`: a copy | Yes | program |
| `static char buf[64];` | `.bss` (zero-filled) | Yes | program |
| `malloc(n)` / `strdup` | heap | Yes | until `free` |

**Why does writing to a literal crash?** The loader maps `.rodata` pages
read-only. A store to one of those pages makes the MMU raise a page fault. The
kernel looks up the VMA, sees no write permission and sends SIGSEGV. The program
prints each address and finds its line in `/proc/self/maps` to show the `r--p`
permission. It then performs the write in a `fork()`ed child, so the parent can
watch the crash and still exit 0.

**Why does `char *p = "hi"` even compile in C?** For historical reasons the
type of a literal in C is `char[N]`, not `const char[N]`. In C++ it is
`const char[N]`, so the same line is an error there. Write `const char *` in C
too (`-Wwrite-strings` makes gcc enforce this).

**Literal merging.** Two identical literals may share one address (gcc's
`-fmerge-constants` does this, and the linker merges `.rodata.str*` sections
across files). Whether they do is *unspecified*, so compare string contents with
`strcmp`, never with `==`.

**`sizeof` vs `strlen`.**

```
 char buf[64] = "abc";   sizeof buf = 64   (array object, compile time)
                         strlen(buf) = 3   (walk to '\0', run time)
 const char *p = "abc";  sizeof p   = 8   (a POINTER!)
 sizeof "abc"            = 4               (char[4], includes '\0')
 char e[] = "abc\0def";  sizeof e = 8, strlen(e) = 3  ← stops at first NUL
```

The classic bug is `void f(char buf[]) { sizeof buf; }`. An array parameter
*decays* to a pointer, so this gives 8. Always pass the size as a separate
argument.

### 3.2 The standard library from scratch

Program: [`c/11_libc_from_scratch.c`](../c/11_libc_from_scratch.c). Every
`my_*` function is checked against libc.

```c
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return p - s; }
char  *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }
int    strcmp(const char *a, const char *b)   /* compare as UNSIGNED char */
{ while (*a && *a == *b) a++, b++; return (unsigned char)*a - (unsigned char)*b; }
```

| Function | Trap you must remember |
|---|---|
| `strcpy` | Doesn't know the size of `dst`, so it can overflow. |
| `strncpy(d,s,n)` | If `strlen(s) >= n` it writes **no `'\0'`**. If shorter, it **pads** all n bytes with zeros (slow for big n). It was designed for fixed-width records, not C strings. |
| `strlcpy` (BSD, glibc ≥ 2.38) | Always terminates. Returns `strlen(src)`, so `ret >= size` means it truncated. |
| `strcat` | Finds the end of `dst` first, so concatenating in a loop is O(n²). |
| `strncat(d,s,n)` | `n` is the maximum number of chars **to append**, not the buffer size. Pass `size - strlen(d) - 1`. |
| `strcmp` | Returns <0, 0 or >0, not necessarily -1/0/1. Compares as `unsigned char`. |
| `strchr(s, '\0')` | Legal: returns a pointer to the terminator. |
| `strstr` | The naive version is O(n·m). glibc uses Two-Way, which is O(n+m). |
| `memset(ints, 1, …)` | Sets **bytes**, so each int becomes `0x01010101`. |
| `strtok` | Writes `'\0'` into your string, keeps a **hidden static cursor** (not reentrant, not thread-safe) and silently drops empty fields. Use `strtok_r`, `strsep` or a manual `strcspn` loop. |
| `atoi` | Has no error reporting (`"abc"` → 0), and overflow is UB. Use `strtol`. |
| `strtol` | The full recipe: set `errno = 0`, then check `end == s` (no digits), `errno == ERANGE` (overflow) and `*end != '\0'` (trailing garbage). |
| `itoa` | Not standard. Use `snprintf`. To write it yourself, fill digits backwards and work with the negative value so that `INT_MIN` works. |

How `strtok` destroys and remembers:

```
 char s[] = "a,b,,c";
 strtok(s, ",")    → "a"   s: [a][\0][b][,][,][c][\0]     static save → 'b'
 strtok(NULL, ",") → "b"   s: [a][\0][b][\0][,][c][\0]    static save → ','
 strtok(NULL, ",") → "c"   (skips the empty field)
 strtok_r(s, ",", &save)  — same algorithm, YOU own `save` → reentrant
```

### 3.3 `memcpy` vs `memmove`

Program: [`c/12_memcpy_vs_memmove.c`](../c/12_memcpy_vs_memmove.c)

`memcpy` assumes the two ranges don't overlap, so it is free to copy in any
order and in any chunk size. If they do overlap, the behaviour is undefined, and
ASan reports `memcpy-param-overlap`. `memmove` checks which way the ranges
overlap and copies in the safe direction:

```
 shift RIGHT (dst > src): copy BACKWARDS        shift LEFT (dst < src): copy FORWARDS
   src: [a][b][c][d]                              src:       [c][d][e][f]
   dst:       [ ][ ][ ][ ]                        dst: [ ][ ][ ][ ]
   last byte first, so nothing is                 first byte first is safe
   overwritten before it is read

 What a naive FORWARD copy does when shifting right by 2:
   "abcdef" → i=0 a[2]=a  i=1 a[3]=b  i=2 a[4]=a[2] — already overwritten!
   result "abababab"  (it re-reads its own output)
```

Use `memmove` whenever you insert into or delete from the middle of a buffer
(shifting the tail). Use `memcpy` when the source and destination are different
objects.

### 3.4 How the real libc is fast: word-at-a-time

Program: [`c/13_word_at_a_time_strlen.c`](../c/13_word_at_a_time_strlen.c)

Instead of loading one byte at a time, load one 64-bit word and test all 8 bytes
for zero at once:

```
 haszero(x) = (x - 0x0101010101010101) & ~x & 0x8080808080808080

 per byte b:      b-1 borrows to 0xFF only if b==0 → top bit set
                  & ~b removes bytes whose top bit was already set (0x81..0xFF)
                  & 0x80 keeps only the flag bit
    b=0x00 → 0x80 (flag)   b=0x41 → 0x00   b=0x80 → 0x00   b=0xC3 → 0x00
```

The result is nonzero exactly when some byte is zero. On little-endian machines
(x86, arm64) the **lowest** flagged byte is the first zero byte, so
`__builtin_ctzll(mask) / 8` gives its index. Flags above a real zero can be
false positives caused by the borrow, but they never come *before* it.

The algorithm: step bytewise until the pointer is 8-aligned, then step 8 bytes
at a time. Why is reading past the `'\0'` allowed? An aligned 8-byte load can
never cross a 4 KiB page boundary, so if the first byte is mapped, all 8 are.
glibc writes this in assembly and goes further with SIMD (SSE2/AVX2/AVX-512 on
x86, ASIMD/SVE on arm64), handling 16–64 bytes per step. The same technique
speeds up `memchr`, `strchr`, `memcmp` and the kernel's
`strncpy_from_user` (`include/asm-generic/word-at-a-time.h`).

### 3.5 Dynamic strings

Program: [`c/14_string_builder.c`](../c/14_string_builder.c)

```
 typedef struct { char *buf; size_t len, cap; } StrBuf;

 StrBuf ──► buf ──► [h][e][l][l][o][\0][?][?]     invariant: buf[len] == '\0'
            len = 5   cap = 8
```

- **Append is O(piece)**, not O(total), because `len` says where the end is.
- **Grow by doubling.** For N appended bytes, the realloc copies total
  1+2+4+…+N < 2N bytes, so each append costs amortised O(1). Growing by a
  constant amount (+16) instead costs O(N²) in total.
- `appendf` calls `vsnprintf` twice: once to measure and once to write. A
  `va_list` can only be walked once, so the second pass needs a `va_copy`.
- `realloc` may **move** the buffer, so any pointer into the old buffer is
  invalid after an append. This is the same rule as for `std::vector`
  iterators.
- Because the struct stores the length, it can also hold binary data that
  contains `'\0'` bytes.

The same design appears in real code: GLib `GString`, Redis `sds`, the
kernel's `seq_buf`, and `std::string` itself.

### 3.6 How `std::string` works inside

Program: [`cpp/10_std_string_internals.cpp`](../cpp/10_std_string_internals.cpp)
counts every heap allocation by replacing the global `operator new`.

```
 libstdc++ (GCC), sizeof(std::string) == 32:

 short ("hi"):  ┌──────────┬────────┬──────────────────────────┐
                │ ptr ─┐   │ size=2 │ h i \0 . . . . (16 bytes)│  ← SSO buffer
                └──────┼───┴────────┴──▲───────────────────────┘
                       └───────────────┘   ptr points INTO the object: 0 allocs

 long (100 x): ┌──────────┬──────────┬──────────────┐
               │ ptr ─────┼► heap [x x x … x \0]    │  capacity stored in the
               │ size=100 │ cap=100  │ (unused)     │  same 16 bytes (a union)
               └──────────┴──────────┴──────────────┘
```

| Library | `sizeof` | Chars stored inline (SSO) |
|---|---|---|
| libstdc++ (GCC) | 32 | 15 |
| libc++ (Clang) | 24 | 22 |
| MSVC STL | 32 | 15 |

What the program measures:

- A string of 15 chars or fewer makes **0 allocations**, and `data()` points
  inside the object. From 16 chars on, the string makes 1 allocation and the
  characters are on the heap.
- `push_back` growth makes capacity go 15 → 30 → 60 → 120…, so 1000 pushes take
  7 allocations. With `reserve(1000)` first, it takes 1.
- **Copying** a long string makes 1 allocation and copies O(n) bytes.
  **Moving** makes 0 allocations: it steals the pointer, which is O(1). The
  moved-from string is left "valid but unspecified" (empty in practice).
- `s += x` in a loop makes about 7 allocations, while `s = s + x` makes about
  200, because it builds a new temporary string every time.

### 3.7 `std::string_view`

Program: [`cpp/11_string_view.cpp`](../cpp/11_string_view.cpp)

A `string_view` is a `(pointer, length)` pair that refers to characters someone
else owns. Copying it, taking a `substr`, or calling `remove_prefix` or
`remove_suffix` is O(1) and allocates nothing. It is the best type for
**read-only parameters**, because it accepts a `std::string`, a literal or a
`char[]` without making a copy.

Two rules:

1. **It is not NUL-terminated.** `printf("%s", sv.data())` reads past the end
   of the view. Use `printf("%.*s", (int)sv.size(), sv.data())`, or build
   `std::string(sv)` when a C API needs a C string.
2. **It does not keep its characters alive.** These all dangle:

```cpp
std::string_view f() { std::string s = "x"; return s; }   // s dies at }
std::string_view v = std::string("a") + "b";             // temporary dies at ;
std::string o = "short"; std::string_view w = o;
o += "...now longer than 15 chars...";                   // realloc → w dangles
```

Use `string_view` for parameters and for slicing within a function. Use
`std::string` for anything you store.

### 3.8 `std::string` in practice

Program: [`cpp/12_c_str_lifetime_and_api.cpp`](../cpp/12_c_str_lifetime_and_api.cpp)

- **`c_str()` lifetime.** The pointer stays valid only until the next operation
  that modifies the string (anything that may reallocate) or until the string is
  destroyed. Fetch it again after any change. Never keep `c_str()` of a
  temporary.
- **Letting a C API fill a string.** Call `resize(n)` and pass
  `s.data(), n + 1`. In C++17 `data()` is non-const, and the byte at
  `data()[size()]` is a `'\0'` slot that may be overwritten with `'\0'`.
- **Embedded NULs.** `std::string("ab\0cd", 5)` has size 5, but
  `std::string("ab\0cd")` has size 2 because the `const char*` constructor stops
  at the NUL.
- **Numbers.** `stoi` throws an exception on bad input but silently accepts
  trailing garbage (`"123xyz"` → 123). `std::from_chars` throws nothing, doesn't
  allocate, ignores the locale, and returns the end pointer, so you can reject
  garbage. Prefer it.
- **Characters.** `toupper` and `isalpha` need an `unsigned char` value. A plain
  `char` of 0x80 or above is negative, and passing it is UB.

### 3.9 Encoding: bytes vs characters

Program: [`c/15_utf8.c`](../c/15_utf8.c)

ASCII uses 7 bits per character, so a byte equals a character. That stops being
true for any other character. UTF-8 uses 1–4 bytes per **code point**:

```
 U+0000..U+007F     0xxxxxxx                                   1 byte  (ASCII)
 U+0080..U+07FF     110xxxxx 10xxxxxx                          2 bytes
 U+0800..U+FFFF     1110xxxx 10xxxxxx 10xxxxxx                 3 bytes
 U+10000..U+10FFFF  11110xxx 10xxxxxx 10xxxxxx 10xxxxxx        4 bytes

 'é' U+00E9 = 00011 101001 → 110_00011 10_101001 = C3 A9
 "café ✓ 😀" = 63 61 66 C3A9 20 E29C93 20 F09F9880   → strlen 14, 8 code points
```

Why UTF-8 won:

- Every ASCII string is already valid UTF-8.
- Bytes below 0x80 (`'\0'`, `'/'`, `'\n'`) never appear inside a multi-byte
  character, so `strlen`, `strchr(s, '/')` and path handling keep working.
- It is self-synchronising: a continuation byte always looks like `10xxxxxx`, so
  you can find the start of a character from anywhere in the string.

Consequences:

- `strlen` counts **bytes**, and `s[i]` is a byte, possibly half of a character.
- Reversing a string byte by byte breaks multi-byte characters. Reverse each
  character's bytes first, then the whole string.
- To count code points in valid UTF-8, count the bytes that are not
  `10xxxxxx`.
- A **validator must be strict**. It has to reject:
  - overlong forms (`C0 AF` decoding to `/` is a classic path-traversal bypass),
  - surrogates (D800–DFFF),
  - values above 10FFFF,
  - truncated sequences,
  - stray continuation bytes.
- What users see as one "character" can be several code points (`e` + a
  combining accent, or emoji with modifiers). That level is called grapheme
  clusters and needs a library such as ICU.
- `wchar_t` is 4 bytes (UTF-32) on Linux and 2 bytes (UTF-16) on Windows. On
  Linux, use `char` + UTF-8 everywhere: the kernel, GLib and GStreamer all do.

### 3.10 Formatting and security

Programs: [`c/16_safe_copy_and_format.c`](../c/16_safe_copy_and_format.c),
[`c/17_stack_smash_bug.c`](../c/17_stack_smash_bug.c) (intentionally broken)

**snprintf is the copy and format tool to use.** It always writes the
terminator and returns the length it **wanted** to write:

```c
int n = snprintf(buf, sizeof buf, "%s:%d", host, port);
if (n < 0)                      /* encoding error */;
else if ((size_t)n >= sizeof buf) /* TRUNCATED — n+1 bytes were needed */;
int need = snprintf(NULL, 0, fmt, ...);   /* measure first, then malloc(need+1) */
```

**How `printf` works: varargs.** The caller passes the extra arguments in
registers and on the stack. `va_list` is a cursor over them, and
`va_arg(ap, int)` means "take the next one, I promise it is an int". **No type
information exists at run time**: the format string is the only description of
what was passed. Arguments are promoted on the way in (`char`/`short` → `int`,
`float` → `double`). `mini_snprintf` in `c/16` implements `%d %u %x %s %c %%`
this way.

**Format-string vulnerability.** `printf(user_input)` lets the user write the
format. `%x %x %x` then prints stack and register words (leaking the canary,
return addresses and ASLR bases), `%s` dereferences garbage, and `%n` **writes**
to memory, which makes it an arbitrary-write primitive. The fix is always
`printf("%s", user_input)`. `-Wformat-security` (part of `-Wformat=2`) warns,
and `_FORTIFY_SOURCE=2` rejects `%n` in writable format strings.

**Stack buffer overflow and canaries.**

```
 higher ┌──────────────────┐
        │ return address   │ ← the attacker wants to overwrite this
        │ saved rbp        │
        │ CANARY (random)  │ ← checked just before `ret`
        │ char buf[16]     │ ← strcpy(buf, long) writes upwards
 lower  └──────────────────┘
```

`-fstack-protector-strong` (on by default in Ubuntu's gcc) places a random
canary between local arrays and the saved registers. If the canary has changed
when the function returns, the program prints
`*** stack smashing detected ***` and aborts with SIGABRT. The canary only
detects the damage when the function returns. It doesn't protect other locals
or heap buffers, and an attacker who leaks the canary can write it back
unchanged. ASan instead reports the **first** bad byte, with a stack trace. The
other layers of defence are NX stacks, ASLR, `_FORTIFY_SOURCE` (checked
`__strcpy_chk` for known buffer sizes) and, best of all, code that never needs
any of them.

### 3.11 String algorithms

Programs: [`c/18_string_algorithms.c`](../c/18_string_algorithms.c),
[`cpp/13_string_algorithms.cpp`](../cpp/13_string_algorithms.cpp)

| Problem | Technique | Cost |
|---|---|---|
| Palindrome (ignoring punctuation and case) | Two pointers moving inwards | O(n), O(1) |
| Anagram | 256-entry count array: +1 for `a`, −1 for `b`, check all zero | O(n), O(1) |
| All anagram positions | Fixed sliding window plus a counter of equal entries | O(n) |
| Reverse the words | Reverse the whole string, then reverse each word | O(n), O(1) |
| Longest palindromic substring | Expand around each of the 2n−1 centres | O(n²), O(1) (Manacher: O(n)) |
| Rotation check | `t` occurs in `s+s` | O(n) with KMP |
| Pattern search | KMP / Rabin-Karp / Boyer-Moore-Horspool | O(n+m) / expected O(n+m) / sublinear on average |

**KMP in one picture.** `fail[i]` is the length of the longest proper prefix of
`p[0..i]` that is also a suffix of it. After a mismatch with `j` characters
matched, continue as if only `fail[j-1]` were matched. The text index never
moves backwards.

```
 pat : a b a b c a b a b d
 fail: 0 0 1 2 0 1 2 3 4 0

 text: a b a b a b c a b a b d
 pat : a b a b c                  mismatch at j=4
           a b a b c              j = fail[3] = 2: the "ab" we already matched
                                   is reused; text index unchanged
```

**Rabin-Karp** gives every window a numeric hash and updates it in O(1) as the
window slides: `h' = (h − lead·B^(m−1))·B + new (mod M)`. When the hashes match,
it compares the actual characters to rule out a collision.
**Boyer-Moore-Horspool** compares from the pattern's end and, on a mismatch,
skips ahead by an amount looked up for the text byte under the pattern's last
position. C++17 provides it as `std::boyer_moore_horspool_searcher`.

---

## 4. Complexity table

| Operation | C string | `std::string` | `string_view` | Why |
|---|---|---|---|---|
| length | O(n) `strlen` | O(1) `size()` | O(1) | C doesn't store the length |
| index `s[i]` | O(1) | O(1) | O(1) | contiguous bytes |
| copy | O(n) | O(n) + 0/1 allocation | O(1) | a view copies only (ptr, len) |
| move | n/a | O(1) for long strings, ≤15 bytes for SSO | O(1) | steals the pointer |
| append k chars | O(n + k) `strcat` | amortised O(k) | n/a | C must find the end first |
| insert/erase in the middle | O(n) `memmove` | O(n) | n/a | the tail shifts |
| substring | O(k) + buffer | O(k) + maybe an allocation | O(1) | a view only adjusts (ptr, len) |
| compare | O(min len) | O(min len) | O(min len) | byte by byte |
| find a char | O(n) | O(n) | O(n) | linear scan |
| find a pattern | naive O(nm), glibc Two-Way O(n+m) | implementation-defined (naive-ish) | same | use KMP/BMH for guarantees |
| UTF-8 code point count | O(n) | O(n) | O(n) | variable-width encoding |
| `strtok` / split | O(n) | O(n) | O(n), no allocations | |

---

## 5. C vs C++

| Need | C | C++ |
|---|---|---|
| Owning, growable text | your own `StrBuf` (`c/14`), GLib `GString` | `std::string` |
| Read-only parameter | `const char *` (+ `size_t len` for binary data) | `std::string_view` |
| Bounded copy | `snprintf(d, n, "%s", s)`, `strlcpy` | `std::string` assignment (cannot overflow) |
| Formatting | `snprintf` (check the return value) | `std::to_string`, `std::ostringstream`, C++20 `std::format` |
| Parse a number | `strtol` + errno / `end` checks | `std::from_chars` (or `stoi`, which throws) |
| Search | `strstr`, `memmem` | `find`, `std::search` + `boyer_moore_horspool_searcher` |
| Tokenise | `strtok_r`, `strsep`, `strcspn` | `string_view::find` loop, `getline` with a delimiter |
| Pass to a C API | it already is a C string | `s.c_str()`, valid until the next modification |

---

## 6. Where it is used in the real world

- **Linux kernel**:
  - `lib/string.c` holds the generic `strlen`, `strscpy` and friends. The kernel
    deprecated `strcpy`, `strncpy` and `strlcpy` in favour of `strscpy`, which
    returns `-E2BIG` on truncation.
  - `word-at-a-time.h` provides the zero-byte trick.
  - `seq_buf` and `seq_file` are string builders.
  - The `%pS`, `%pI4` and `%pOF` printk extensions show that format strings can
    be extended.
- **glibc / musl**: SIMD `str*` functions selected at run time (IFUNC) for your
  CPU.
- **GStreamer / GLib**:
  - `GString`, `g_strdup_printf`, `g_utf8_validate`.
  - Caps and pipeline descriptions are strings parsed at run time
    (`gst_parse_launch`).
  - GLib requires UTF-8 everywhere.
- **Security**: format-string bugs and `strcpy` overflows fill decades of CVEs.
  Stack canaries, FORTIFY_SOURCE and ASan are the industrial responses.
- **Databases / Redis**: `sds` (length-prefixed, binary-safe strings), and
  interned strings in symbol tables.
- **Compilers**: string interning (one copy of each identifier), literal pooling
  into `.rodata` (the merging you saw in `c/10`).
- **Embedded**: AT-command parsers, NMEA/GPS sentences, U-Boot environment
  variables (`setenv bootargs …`), and sysfs attributes are all short strings
  parsed with `strtoul` and friends, often without heap allocation.
- **grep / text editors**: Boyer-Moore variants, SIMD `memchr`, UTF-8 aware
  cursor movement.

---

## 7. Common mistakes and pitfalls

1. Writing to a string literal (`char *p = "x"; p[0] = 'y';`) causes SIGSEGV.
   Declare literals `const char *`.
2. Forgetting the `+1` for `'\0'`: `malloc(strlen(s))` is one byte short.
3. `strncpy` leaves no terminator when the source is long enough. Terminate
   manually or use `snprintf`.
4. Passing the buffer size as `strncat`'s `n`, when `n` means "chars to append".
5. `sizeof` on a pointer or array parameter gives 8, not the buffer size.
6. Calling `strlen` in the loop condition makes the loop O(n²).
7. Comparing strings with `==` compares addresses. Use `strcmp`, or `==` on
   `std::string`.
8. Ignoring `snprintf`'s return value means silent truncation.
9. `printf(user)` is a format-string vulnerability. Write `printf("%s", user)`.
10. Using `strtok` in nested loops or threads. Use `strtok_r`.
11. Using `atoi` on untrusted input gives no error and UB on overflow.
12. `memcpy` with overlapping ranges is UB. Use `memmove`.
13. Passing a plain `char` (possibly negative) to `isalpha`/`toupper` is UB.
    Cast to `unsigned char`.
14. Treating bytes as characters: `strlen` and `s[i]` on UTF-8, or reversing it
    byte by byte.
15. Keeping `c_str()` or `string_view` pointers across modifications, or to
    temporaries.
16. Passing `sv.data()` where a NUL-terminated string is expected.
17. Building a string with `s = s + x` in a loop. Use `+=` and `reserve`.
18. `std::string(const char*)` stops at the first embedded `'\0'`. Use the
    `(ptr, len)` constructor.

---

## 8. Files in this lesson (strings part)

| File | What it shows | Run |
|---|---|---|
| [c/10_string_memory_layout.c](../c/10_string_memory_layout.c) | `.rodata`/`.data`/stack/heap addresses against `/proc/self/maps`, literal merging, `sizeof` vs `strlen`, the literal-write crash in a child process | `make run P=dsa/05-arrays-and-strings/c/10_string_memory_layout.c` |
| [c/11_libc_from_scratch.c](../c/11_libc_from_scratch.c) | strlen, strcpy, strncpy, strlcpy, strcat, strcmp, strchr, strstr, memset, strtok/strtok_r, atoi, strtol checks, itoa | `make run P=dsa/05-arrays-and-strings/c/11_libc_from_scratch.c` |
| [c/12_memcpy_vs_memmove.c](../c/12_memcpy_vs_memmove.c) | overlap corruption drawn and shown, `memmove`'s copy direction, insert/erase in a buffer | `make run P=dsa/05-arrays-and-strings/c/12_memcpy_vs_memmove.c` |
| [c/13_word_at_a_time_strlen.c](../c/13_word_at_a_time_strlen.c) | the has-zero-byte trick proved, 8-bytes-per-step `strlen` checked at every alignment | `make run P=dsa/05-arrays-and-strings/c/13_word_at_a_time_strlen.c` |
| [c/14_string_builder.c](../c/14_string_builder.c) | growable `StrBuf`: doubling, `appendf` via `vsnprintf`, insert, detach, binary-safe | `make run P=dsa/05-arrays-and-strings/c/14_string_builder.c` |
| [c/15_utf8.c](../c/15_utf8.c) | UTF-8 encode, strict decode/validate, count, aware reverse, `wchar_t` | `make run P=dsa/05-arrays-and-strings/c/15_utf8.c` |
| [c/16_safe_copy_and_format.c](../c/16_safe_copy_and_format.c) | copy-family comparison, `snprintf` truncation detection, a varargs `mini_snprintf`, format-string bugs, canaries | `make run P=dsa/05-arrays-and-strings/c/16_safe_copy_and_format.c` |
| [c/17_stack_smash_bug.c](../c/17_stack_smash_bug.c) | **intentional bug**: stack overflow. ASan reports it; without ASan, the canary aborts on return | `make run P=dsa/05-arrays-and-strings/c/17_stack_smash_bug.c` |
| [c/18_string_algorithms.c](../c/18_string_algorithms.c) | palindrome, anagram, reverse words, KMP (checked against naive search) | `make run P=dsa/05-arrays-and-strings/c/18_string_algorithms.c` |
| [cpp/10_std_string_internals.cpp](../cpp/10_std_string_internals.cpp) | SSO proved with an allocation counter, growth, copy vs move, `+=` vs `s = s + x` | `make run P=dsa/05-arrays-and-strings/cpp/10_std_string_internals.cpp` |
| [cpp/11_string_view.cpp](../cpp/11_string_view.cpp) | zero-allocation slicing, trim, split and parse; not NUL-terminated; dangling rules | `make run P=dsa/05-arrays-and-strings/cpp/11_string_view.cpp` |
| [cpp/12_c_str_lifetime_and_api.cpp](../cpp/12_c_str_lifetime_and_api.cpp) | `c_str()` lifetime, a C API writing into a `std::string`, embedded NULs, API costs, `stoi` vs `from_chars` | `make run P=dsa/05-arrays-and-strings/cpp/12_c_str_lifetime_and_api.cpp` |
| [cpp/13_string_algorithms.cpp](../cpp/13_string_algorithms.cpp) | KMP, Rabin-Karp and BMH (`std::search`) cross-checked; anagram positions; longest palindrome; rotation | `make run P=dsa/05-arrays-and-strings/cpp/13_string_algorithms.cpp` |

To see the canary instead of ASan in `c/17`, build it by hand without sanitizers:

```sh
gcc -std=c11 -O0 -fstack-protector-strong dsa/05-arrays-and-strings/c/17_stack_smash_bug.c -o /tmp/smash && /tmp/smash
```

---

## 9. Exercises

1. **(easy)** Write `my_strrchr` and `my_strncmp`, and test them against libc
   with the pattern used in `c/11`.
2. **(easy)** Write `str_trim_inplace(char *s)`: remove leading and trailing
   whitespace using `memmove`. *Hint: find the first non-space, then the last,
   and move once.*
3. **(easy)** Write `count_words(const char *s)` with `strtok_r` on a copy, then again with only
   `strspn`/`strcspn` (no writes to the input).
4. **(medium)** Add `sb_replace_all(StrBuf *, from, to)` to `c/14` in a single
   pass, building into a second buffer. Why is replacing in place harder when
   `to` is longer than `from`?
5. **(medium)** Write `memchr_word()` using the haszero trick: XOR each word
   with the target byte repeated 8 times (`c * 0x0101010101010101`) first.
6. **(medium)** Write `utf8_truncate(char *s, size_t max_bytes)` so that it never
   cuts a character in half. *Hint: back up over `10xxxxxx` bytes.*
7. **(medium)** Write a hex dump function (`xxd`-style) with `snprintf` into a
   fixed line buffer, checking every return value.
8. **(hard)** Write a strict `parse_ipv4(const char *, uint32_t *)` that rejects
   `"1.2.3"`, `"1.2.3.256"`, `"01.2.3.4"` and `"1.2.3.4 "`. Compare it with
   `inet_pton`.
9. **(hard)** Add a small-buffer optimisation to `StrBuf`: store up to 23 bytes
   inside the struct, with a flag saying which mode it is in. Count `malloc`
   calls before and after.
10. **(hard)** Implement Manacher's algorithm (longest palindrome in O(n)) and
    cross-check it against the expand-around-centre version in `cpp/13`.
11. **(hard)** Implement Aho-Corasick (KMP for many patterns at once, the
    algorithm behind `fgrep` and intrusion-detection systems).

---

## 10. Remember forever

```
C string = bytes + '\0'. No length, no capacity → strlen O(n), nobody knows dst size.
Literals live in .rodata (r--): never write them; char s[] = "x" makes a writable COPY.
sizeof array = bytes reserved; sizeof pointer = 8; strlen = bytes before first '\0'.
Copy safely: snprintf(d, n, "%s", s); truncated iff ret >= n. strncpy may not terminate.
Overlap → memmove (picks direction). strtok has a hidden static → strtok_r.
strtol: errno=0, end==s? ERANGE? *end? — or from_chars in C++.
Fast strlen: (x - 0x01..01) & ~x & 0x80..80 ≠ 0  ⇔  some byte is zero.
Builder: {buf,len,cap}, double on grow → amortised O(1); keep buf[len]=='\0'.
std::string: ptr/size/cap + SSO (≤15 chars inside, 0 allocs). Move steals, copy allocates.
string_view = (ptr,len), borrows, not NUL-terminated. UTF-8: strlen counts BYTES.
printf(user) is a vuln. Canary sits below the return address, checked on return.
```

Review this card on the schedule in [LEARNING_METHOD.md](../../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Where are the bytes of `"hello"` stored, and what exactly happens, from the
   MMU up to the signal, when you write to one of them?
2. What is the difference between `char s[] = "hi";` and `const char *s = "hi";`
   in memory, writability and `sizeof`?
3. Why is `strlen` O(n), and why does `for (i = 0; i < strlen(s); i++)` become
   O(n²)?
4. Give two ways `strncpy` surprises people. What does the return value of
   `strlcpy` or `snprintf` let you do that `strncpy` doesn't?
5. Draw what goes wrong if you shift `"abcdef"` right by 2 with a forward byte
   copy. Which direction does `memmove` choose, and why?
6. Explain each part of `(x - 0x0101…) & ~x & 0x8080…`. Why is reading a whole
   aligned word past the terminator safe on real hardware?
7. Why does growing a buffer by doubling give amortised O(1) appends, while
   growing by +16 does not?
8. What is SSO? How would you prove that a given string uses it, without
   reading the library source?
9. Why is moving a long `std::string` O(1)? What state is the moved-from string
   in?
10. Name three ways to create a dangling `string_view`.
11. Why is `printf("%s", sv.data())` a bug?
12. How many bytes does `é` take in UTF-8, and what are they? Why can't a valid
    UTF-8 string contain `C0 AF`?
13. What do `%x` and `%n` do in an attacker-controlled format string?
14. Where does the stack canary sit, when is it checked, and what can it not
    protect?
15. In KMP, what does `fail[i]` mean, and why does the text index never move
    backwards?
