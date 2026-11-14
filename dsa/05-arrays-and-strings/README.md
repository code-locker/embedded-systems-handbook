# 05 — Arrays and strings

> An array is **N same-sized boxes packed side by side in memory**. Because
> they are side by side, box `i` is at `base + i * size` — one multiply, one
> add, done. Every strength and every weakness of arrays follows from that
> one fact.
>
> Strings are arrays of bytes with a convention on top. They get their own
> in-depth sub-lesson: **[notes/strings.md](notes/strings.md)** (where string
> bytes live, libc from scratch, `std::string` internals and SSO, UTF-8,
> format-string and buffer-overflow security). This README covers arrays,
> dynamic arrays and the classic array/string techniques, and summarises the
> strings part in section 3.8.

---

## 1. Why this exists

Almost every program needs "a bunch of the same thing": the samples in an
audio buffer, the pixels in a frame, the bytes of a UART packet, the rows of
a lookup table. The array is the simplest possible answer and, on real
hardware, usually the **fastest**:

- **O(1) random access**: `a[i]` is computed, not searched for.
- **Cache-friendly**: neighbours are in the same 64-byte cache line, and the
  CPU's prefetcher sees the linear pattern coming. Walking an array is the
  fastest thing a CPU can do with memory.
- **Zero overhead**: no pointers, no headers per element. 1000 `int`s are
  exactly 4000 bytes — what a DMA engine, a framebuffer or a network card
  expects.

The cost: the size is fixed at the moment you allocate, and inserting in the
middle means shifting everything after it. The **dynamic array** (C's
hand-made vector, C++'s `std::vector`) fixes the first problem with a clever
growth rule; the second is the price you pay — and usually it is worth it.

## 2. Mental model

**A row of numbered lockers in a corridor.** Every locker is the same width.
To find locker 37 you do not open lockers 0..36 — you walk straight to
`37 × width` metres from the start. To add a locker in the middle, though,
everyone after it has to move one place along.

```
  int a[5] = {10, 20, 30, 40, 50};

  index:        0       1       2       3       4
             +-------+-------+-------+-------+-------+
  a  ------> |  10   |  20   |  30   |  40   |  50   |
             +-------+-------+-------+-------+-------+
  address:   0x1000  0x1004  0x1008  0x100c  0x1010
                     <-4 B->

  &a[i]  ==  (char *)a + i * sizeof(int)        <- the whole secret
  a[i]   ==  *(a + i)  ==  i[a]   (yes, really: addition commutes)
```

A **dynamic array** is the same row of lockers, plus a note saying "5 used,
8 built". When all 8 are full, it builds a new corridor **twice as long**,
moves everyone over, and demolishes the old one.

```
   struct { int *data; size_t len; size_t cap; }
   +---------+       +----+----+----+----+----+----+----+----+
   | data ---------> | 3  | 1  | 4  | 1  | 5  | ?  | ?  | ?  |
   | len = 5 |       +----+----+----+----+----+----+----+----+
   | cap = 8 |        <-------- len --------> <--- spare --->
   +---------+
```

## 3. How it works

### 3.1 Static arrays and memory layout

- `sizeof(a)` on a real array gives the **total bytes**; `sizeof(a)/sizeof(a[0])`
  gives the count (the `ARRAY_SIZE` macro).
- **Decay**: in almost every expression, and *always* when passed to a
  function, an array turns into a pointer to its first element. Inside the
  function, `sizeof(param)` is `sizeof(int *)` — 8 on the BeagleBone's A72.
  So C functions take `(const int *a, size_t n)`. There is no way around it.
- `&a` is the same address as `a` but a different type: `int (*)[5]`,
  "pointer to the whole array". `&a + 1` jumps 20 bytes.
- Initialisation: `int a[6] = {1, 2};` zero-fills the rest; `{[4] = 7}`
  (designated initialiser) sets one slot; `static` arrays are zero (`.bss`).
- Arrays cannot be assigned (`a = b` is an error) — use `memcpy`, or wrap the
  array in a struct, which *is* assignable.
- Where it lives decides its lifetime: a local array is on the **stack**
  (gone at `}`; keep it small — the default thread stack is 8 MiB, a kernel
  stack only 16 KiB), a `static`/global one in `.data`/`.bss` (lives forever),
  a `malloc`'d one on the **heap** (lives until `free`).

### 3.2 2D arrays are row-major

C and C++ store `m[ROWS][COLS]` **row after row** in one flat run:

```
  logical 3x4             memory (12 ints, one block)
     c0 c1 c2 c3
  r0  0  1  2  3          | 0 1 2 3 | 4 5 6 7 | 8 9 10 11 |
  r1  4  5  6  7            <-row 0-> <-row 1-> <--row 2-->
  r2  8  9 10 11
                          &m[r][c] == base + (r * COLS + c) * sizeof(int)
```

Consequences you must remember:

1. To pass `m` to a function the compiler needs **COLS** (`int m[][COLS]`)
   because it is part of the address formula. ROWS may be omitted.
2. **Loop order matters.** `for r { for c { m[r][c] } }` walks memory
   linearly; swapping the loops jumps `COLS*4` bytes per step and thrashes the
   cache. `c/01` measures it: same O(n), roughly 2-10x slower.
3. A heap "2D array" is best done as **one block** indexed by hand —
   `buf[y * stride + x]` — exactly how a framebuffer, a V4L2 buffer or a
   GStreamer video frame is laid out (with `stride >= width * bpp` for
   alignment padding). `int **` with one `malloc` per row is N+1 allocations,
   pointer chasing, and rows scattered across the heap.

### 3.3 Dynamic array (vector)

Keep `{data, len, cap}`.

**push_back** — if `len < cap`, write into `data[len++]`: O(1). If full,
allocate `2 * cap`, copy the `len` elements, free the old block, then write.

```
  full (len = cap = 4):   old [a b c d]
  grow:                   new [a b c d . . . .]   copy 4, free old
  write:                  new [a b c d e . . .]   len = 5, cap = 8
```

**Why doubling gives amortised O(1).** Over n pushes the copies happen at
sizes 1, 2, 4, ..., up to n: total `1 + 2 + 4 + ... + n < 2n`. Spread over n
pushes, that is **under 2 copies per push** — a constant. `c/02` measures
1.31 copies/push for 100 000 pushes.

**Why "+10" growth is a disaster.** Copies happen at 10, 20, 30, ... :
total `≈ n²/20`. `c/02` measures **5000 copies per push** for the same
100 000 pushes. Growth must be *geometric* (x2, or x1.5 like MSVC and
folly — 1.5 lets freed blocks be reused by later growth).

**insert / erase at i** — shift the tail with `memmove` (the ranges overlap,
so `memcpy` would be undefined behaviour): O(n − i).
**swap-remove** — if order does not matter, move the last element into the
hole: O(1). Common in game engines and in the kernel.

**Realloc safely:** `p = realloc(v->data, ...)`; if `p == NULL` the old block
is still yours. Writing `v->data = realloc(v->data, ...)` leaks it on failure.
Also check `new_cap * sizeof(T)` for overflow.

**C++ version (`cpp/01`).** Memory beyond `size()` is raw: no object lives
there. So the template allocates with `::operator new`, constructs with
**placement new**, destroys with an explicit `~T()`, and grows by
`std::move_if_noexcept` — which is why your move constructors should be
`noexcept` (otherwise `std::vector` *copies* during growth).

### 3.4 Two pointers

Two indices walk the array; each only moves in one direction, so the total
work is O(n) instead of O(n²) for "try every pair".

- **Opposite ends** (sorted input): pair with sum = target. If the sum is too
  small, the left element cannot pair with anything (the right one was the
  largest left) — discard it: `L++`. Too big: `R--`.
- **Same direction, read/write**: `w` marks where the next keeper goes, `r`
  reads. Remove duplicates, remove zeroes, filter in place — O(n), O(1) space.
- **Three pointers**: Dutch national flag (sort 0/1/2 in one pass) = the
  3-way partition at the heart of quicksort.

```
  target 10, sorted:   [1  3  4  6  8  11]
                        L               R    12 > 10 -> R--
                        L           R        9 < 10  -> L++
                           L        R        11 > 10 -> R--
                           L     R           9 < 10  -> L++
                              L  R           4 + 6 = 10  found
```

### 3.5 Sliding window

A window `[L, R]` slides right. When R moves, add `a[R]`; when L moves,
subtract `a[L]`. Each element enters once and leaves once: **O(n)**.

- **Fixed size k**: max sum of k consecutive — `s += a[R] - a[R-k]`.
- **Variable size**: grow R until the window becomes valid/invalid, then
  shrink L until it flips back. Shortest subarray with sum ≥ target; longest
  substring without repeated characters (`last[c]` table); longest window with
  at most k distinct values (hash map of counts, `cpp/03`).
- Requirement: shrinking must be monotone — e.g. with all-positive numbers,
  shrinking always lowers the sum. With negative numbers the window trick
  breaks; use prefix sums + hash map instead.

```
  k = 3     [2 1 5 1 3 2]
             [-----]          8
               [-----]        8 - 2 + 1 = 7
                 [-----]      7 - 1 + 3 = 9   <- max
                   [-----]    9 - 5 + 2 = 6
```

### 3.6 Prefix sums, difference arrays, Kadane

**Prefix sums**: `P[0] = 0`, `P[i+1] = P[i] + a[i]`. Then
`sum(a[l..r]) = P[r+1] − P[l]` in O(1) after O(n) setup.

```
  a:       3   1   4   1   5
  P:   0   3   4   8   9  14
  sum(a[1..3]) = P[4] - P[1] = 9 - 3 = 6
```

- **2D (summed-area table / integral image)**: any rectangle sum in O(1)
  with four lookups (inclusion–exclusion). Used for box blur, Haar features
  in face detection, and on the TDA4VM's vision accelerators.
- **Prefix sums + hash map**: count subarrays with sum exactly k (works with
  negatives): look up how many earlier prefixes equal `prefix − k`.
- **Difference array** (the inverse): "add v to `a[l..r]`" becomes
  `D[l] += v; D[r+1] −= v` in O(1); one prefix-sum pass recovers `a`.

**Kadane** (maximum-sum subarray, O(n), O(1) space). At each index ask: *is
the best run ending here "the previous run plus me" or "just me"?*
`cur = max(a[i], cur + a[i])`; `best = max(best, cur)`. A negative running
sum can only hurt what follows, so it gets dropped.

```
  a:    -2   1  -3   4  -1   2   1  -5   4
  cur:  -2   1  -2   4   3   5   6   1   5
  best: -2   1   1   4   4   5   6   6   6      -> [4 -1 2 1] = 6
```

### 3.7 Reversal and rotation

**Reverse** in place: swap `a[L]`, `a[R]`, move inward until they meet. Use
half-open `[lo, hi)` with `size_t` so `n == 0` cannot underflow `hi − 1`.

**Rotate left by k with three reversals** — O(n) time, O(1) space:

```
  k = 2:        [1 2 | 3 4 5 6 7]
  reverse A:    [2 1 | 3 4 5 6 7]
  reverse B:    [2 1 | 7 6 5 4 3]
  reverse all:  [3 4 5 6 7 | 1 2]       (Aʳ Bʳ)ʳ = B A
```

Alternatives in `c/05`: naive shift-by-one k times (O(n·k)), copy to a
temporary (O(n) extra space), and the juggling/cycle-leader method (moves
each element exactly once — fewest writes). In C++: `std::rotate`.
Rotation is how you "unwrap" a full ring buffer into oldest-first order.

### 3.8 Strings (summary — full lesson in [notes/strings.md](notes/strings.md))

A C string is a `char` array whose end is marked by a `'\0'` byte. That one
convention explains almost everything:

- `strlen` is **O(n)** — it must walk to find the `'\0'`. `sizeof(buf)` is the
  buffer size; `strlen(buf)` is the text length. Different things.
- `char s[] = "hi";` **copies** 3 bytes into a writable array;
  `char *p = "hi";` points into **read-only `.rodata`** — writing through `p`
  segfaults (`c/10` shows the addresses against `/proc/self/maps`).
- Copying: `strcpy` has no bound (overflow), `strncpy` does **not**
  NUL-terminate when the source is too long (and zero-pads — it was designed
  for fixed-width fields), `snprintf(dst, size, "%s", src)` always
  terminates and **returns the length it wanted**, so `ret >= size` means
  "truncated". Prefer it (or `strlcpy` where available).
- `memcpy` vs `memmove`: overlap needs `memmove`.
- Fast libc functions check **8 bytes at a time** with the "has zero byte"
  trick `(x − 0x0101…01) & ~x & 0x8080…80` (`c/13`).
- `std::string` = `{pointer, size, capacity}` + a small inline buffer:
  strings ≤ 15 chars (libstdc++) live **inside the object** with no heap
  allocation (SSO). `std::string_view` = `{pointer, length}`, non-owning,
  not NUL-terminated, and can dangle.
- UTF-8: one code point = 1–4 bytes; `strlen` counts **bytes**, not
  characters.
- Algorithms: palindrome (two pointers), anagram (count array of 256),
  KMP pattern search in O(n + m) (failure function = "longest proper prefix
  that is also a suffix").

## 4. Complexity table

| Operation | Static array | Dynamic array | Why |
|---|---|---|---|
| `a[i]` read/write | O(1) | O(1) | address = base + i·size |
| Search (unsorted) | O(n) | O(n) | must look at every element |
| Search (sorted) | O(log n) | O(log n) | binary search |
| push_back | — | **amortised O(1)**, worst O(n) | doubling: < 2 copies per push on average |
| pop_back | — | O(1) | just `len--` (C++: run `~T()`) |
| insert / erase at i | O(n − i) | O(n − i) | shift the tail by one |
| swap-remove at i | O(1) | O(1) | move last into the hole; order lost |
| reserve(n) | — | O(len) once | one reallocation |
| Space | n·size | n·size + spare ≤ 2× | geometric growth leaves at most cap/2 unused |
| Two pointers / sliding window | O(n) | | each index moves ≤ n times |
| Prefix-sum build / query | O(n) / O(1) | | P[r+1] − P[l] |
| Kadane | O(n), O(1) space | | one pass, one decision per index |
| Reverse / rotate in place | O(n), O(1) space | | three reversals |
| `strlen`, `strcpy`, `strcmp` | O(n) | | walk to the `'\0'` |
| KMP search | O(n + m) | | text pointer never moves back |

## 5. C vs C++

| Need | C | C++ (use this) |
|---|---|---|
| Fixed-size array | `int a[N]` (decays, no size, not assignable) | `std::array<int, N>` — same bytes, knows `size()`, copyable, comparable, `at()` checks bounds |
| Growable array | hand-made `{data, len, cap}` + `realloc` (`c/02`) | `std::vector<T>` (`cpp/01` builds one; `cpp/02` uses the real one) |
| Pass "an array" | `(const T *a, size_t n)` | `const std::vector<T>&`, `std::array<T,N>&`, or iterators / (C++20) `std::span` |
| 2D | `T m[R][C]` or one block + `y*stride+x` | one `std::vector<T>` + `operator()(r, c)` |
| Text | `char[]` + `'\0'` + `<string.h>` | `std::string`, `std::string_view` |
| Algorithms | write loops; `qsort`, `bsearch` | `<algorithm>`: `std::sort`, `std::reverse`, `std::rotate`, `std::partition`, `std::partial_sum`, `std::search` |

`std::vector` facts to remember: growth factor 2 in libstdc++/libc++ (1.5 in
MSVC); `reserve()` avoids reallocations; anything that may reallocate
**invalidates every pointer, reference and iterator**; `erase` invalidates
from the erased position onward; use the erase–remove idiom; `emplace_back`
constructs in place; `vector<bool>` is bit-packed and returns proxies.

## 6. Where it is used in the real world

- **Linux kernel**: almost everything that is not a list is an array —
  per-CPU arrays, the `sk_buff` data area, `struct page` arrays (`mem_map`),
  syscall tables, `kfifo` (an array used as a ring buffer). `ARRAY_SIZE()`
  in `<linux/kernel.h>` even refuses to compile if you pass it a pointer.
  The kernel's `flex_array`/`xarray` and `krealloc`-grown arrays are the
  dynamic-array idea.
- **Embedded / DMA**: DMA descriptors and buffers must be contiguous arrays —
  the hardware computes addresses exactly like `base + i * size`. On the
  TDA4VM, UDMA rings are arrays of descriptors.
- **Video and GStreamer**: a frame is a 2D row-major array with a *stride*
  (`GstVideoInfo` stores `stride[]` and `offset[]` per plane); `GstBuffer`
  memory is a byte array; summed-area tables power box filters.
- **Audio**: interleaved sample arrays (`L R L R …`), ring buffers in ALSA,
  sliding windows for moving averages and RMS meters.
- **Databases / analytics**: column stores keep each column as a dense array
  so a scan is a linear, SIMD-friendly walk; prefix sums answer range
  aggregates.
- **Networking**: packet parsing with two pointers (header / payload cursor);
  sliding windows in TCP flow control (literally named that).
- **Compilers / games**: `std::vector` everywhere; swap-remove for entity
  lists; struct-of-arrays layouts for cache efficiency.
- **Strings**: every protocol, log line, config file and command line —
  see [notes/strings.md](notes/strings.md#6-where-it-is-used-in-the-real-world).

## 7. Common mistakes and pitfalls

1. **Off-by-one**: `for (i = 0; i <= n; i++)` reads `a[n]`, one past the end.
   ASan reports it as `stack-buffer-overflow` / `heap-buffer-overflow`.
2. **`sizeof` on a pointer**: `ARRAY_SIZE(p)` or `sizeof(param)` inside a
   function gives 8 / 2 or similar garbage. Pass the length.
3. **`hi = n - 1` with `size_t` and `n == 0`** wraps to `SIZE_MAX`. Use
   half-open ranges `[lo, hi)`.
4. **Wrong loop order on 2D data**: column-major traversal of a row-major
   array is correct but cache-hostile.
5. **`v->data = realloc(v->data, …)`** leaks the old block when realloc fails.
6. **Overflow in `n * sizeof(T)`** before `malloc`/`realloc` — check against
   `SIZE_MAX / sizeof(T)` (or use `calloc`, which checks for you).
7. **Additive growth** (`cap += 10`) makes push O(n) amortised.
8. **Keeping a pointer/reference/iterator into a `std::vector` across
   `push_back`** — dangling after reallocation. Keep indices instead.
9. **`it = v.erase(it)` forgotten** — `++it` after `erase` uses an invalid
   iterator.
10. **`memcpy` on overlapping ranges** (inserting/erasing in place) — use
    `memmove`.
11. **Big local arrays** (`int buf[1 << 20];`) overflow the stack, especially
    in threads and in the kernel. Use the heap or `static`.
12. **Sliding window with negative numbers** — the shrink step is no longer
    monotone; use prefix sums + hash map.
13. **Kadane on all-negative input** — initialising `best = 0` returns 0
    (empty subarray) instead of the largest element. Start from `a[0]`.
14. **Integer overflow in sums**: sum `int`s into a `long`/`int64_t`.
15. **Strings**: `strncpy` without manual `'\0'`, writing to a string literal,
    `strlen` in a loop condition (O(n²)), `string_view` to a temporary — see
    [notes/strings.md §7](notes/strings.md#7-common-mistakes-and-pitfalls).

## 8. Files in this lesson

### Arrays and techniques

| File | What it shows | Run |
|---|---|---|
| [c/01_static_arrays_memory.c](c/01_static_arrays_memory.c) | address arithmetic, `sizeof` vs count, decay, init rules, row-major 2D, cache effect of loop order, copying arrays | `make run P=dsa/05-arrays-and-strings/c/01_static_arrays_memory.c` |
| [c/02_dynamic_array.c](c/02_dynamic_array.c) | vector from scratch: push/pop/insert/erase/swap-remove/shrink, doubling vs +10 copy counts | `make run P=dsa/05-arrays-and-strings/c/02_dynamic_array.c` |
| [c/03_two_pointers_sliding_window.c](c/03_two_pointers_sliding_window.c) | pair sum, dedup, Dutch flag, fixed and variable windows, longest unique substring — all vs brute force | `make run P=dsa/05-arrays-and-strings/c/03_two_pointers_sliding_window.c` |
| [c/04_prefix_sums_kadane.c](c/04_prefix_sums_kadane.c) | 1D/2D prefix sums, difference array, Kadane with indices | `make run P=dsa/05-arrays-and-strings/c/04_prefix_sums_kadane.c` |
| [c/05_reverse_and_rotate.c](c/05_reverse_and_rotate.c) | in-place reverse, rotate by three reversals / naive / copy / juggling, ring-buffer unwrap | `make run P=dsa/05-arrays-and-strings/c/05_reverse_and_rotate.c` |
| [cpp/01_vector_template.cpp](cpp/01_vector_template.cpp) | `Vector<T>`: raw storage, placement new, rule of five, `move_if_noexcept`, lifetime counting, fuzzed vs `std::vector` | `make run P=dsa/05-arrays-and-strings/cpp/01_vector_template.cpp` |
| [cpp/02_std_vector_and_array.cpp](cpp/02_std_vector_and_array.cpp) | `std::array`, real growth factor, `reserve`, invalidation, erase–remove, `emplace_back`, contiguous matrix, `vector<bool>` | `make run P=dsa/05-arrays-and-strings/cpp/02_std_vector_and_array.cpp` |
| [cpp/03_array_techniques.cpp](cpp/03_array_techniques.cpp) | 3-sum, k-distinct window, `partial_sum` + subarray-sum-k, template Kadane, `std::rotate`/`stable_partition` | `make run P=dsa/05-arrays-and-strings/cpp/03_array_techniques.cpp` |

### Strings (lesson: [notes/strings.md](notes/strings.md))

| File | What it shows | Run |
|---|---|---|
| [c/10_string_memory_layout.c](c/10_string_memory_layout.c) | `.rodata`/stack/heap addresses vs `/proc/self/maps`, literal merging, `sizeof` vs `strlen`, literal-write crash in a child | `make run P=dsa/05-arrays-and-strings/c/10_string_memory_layout.c` |
| [c/11_libc_from_scratch.c](c/11_libc_from_scratch.c) | strlen … strtok_r, atoi, strtol with error checks, itoa | `make run P=dsa/05-arrays-and-strings/c/11_libc_from_scratch.c` |
| [c/12_memcpy_vs_memmove.c](c/12_memcpy_vs_memmove.c) | overlap corruption drawn and shown, `memmove` direction | `make run P=dsa/05-arrays-and-strings/c/12_memcpy_vs_memmove.c` |
| [c/13_word_at_a_time_strlen.c](c/13_word_at_a_time_strlen.c) | has-zero-byte bit trick, 8-bytes-per-step `strlen` | `make run P=dsa/05-arrays-and-strings/c/13_word_at_a_time_strlen.c` |
| [c/14_string_builder.c](c/14_string_builder.c) | growable string builder `{buf, len, cap}` with `appendf` | `make run P=dsa/05-arrays-and-strings/c/14_string_builder.c` |
| [c/15_utf8.c](c/15_utf8.c) | UTF-8 encode, validate, decode, count; `wchar_t` | `make run P=dsa/05-arrays-and-strings/c/15_utf8.c` |
| [c/16_safe_copy_and_format.c](c/16_safe_copy_and_format.c) | strcpy/strncpy/strlcpy/snprintf, truncation detection, varargs mini-printf, format-string bugs | `make run P=dsa/05-arrays-and-strings/c/16_safe_copy_and_format.c` |
| [c/17_stack_smash_bug.c](c/17_stack_smash_bug.c) | **intentional bug**: stack overflow caught by ASan / the stack canary (not run by `make test`) | `make run P=dsa/05-arrays-and-strings/c/17_stack_smash_bug.c` |
| [c/18_string_algorithms.c](c/18_string_algorithms.c) | palindrome, anagram, reverse words, KMP | `make run P=dsa/05-arrays-and-strings/c/18_string_algorithms.c` |
| [cpp/10_std_string_internals.cpp](cpp/10_std_string_internals.cpp) | SSO proven with an allocation counter, growth, copy vs move | `make run P=dsa/05-arrays-and-strings/cpp/10_std_string_internals.cpp` |
| [cpp/11_string_view.cpp](cpp/11_string_view.cpp) | zero-allocation slicing/splitting, no terminator, dangling rules | `make run P=dsa/05-arrays-and-strings/cpp/11_string_view.cpp` |
| [cpp/12_c_str_lifetime_and_api.cpp](cpp/12_c_str_lifetime_and_api.cpp) | `c_str()` lifetime, C APIs writing into `std::string`, `stoi` vs `from_chars` | `make run P=dsa/05-arrays-and-strings/cpp/12_c_str_lifetime_and_api.cpp` |
| [cpp/13_string_algorithms.cpp](cpp/13_string_algorithms.cpp) | KMP, Rabin-Karp, Boyer-Moore-Horspool, longest palindrome, rotation check | `make run P=dsa/05-arrays-and-strings/cpp/13_string_algorithms.cpp` |

## 9. Exercises

Easy
1. Write `int max_of(const int *a, size_t n)` and call it with `ARRAY_SIZE`.
   Then try `ARRAY_SIZE` on the parameter inside it and explain the result.
2. Add `vec_find(const IntVec *v, int x)` to `c/02` returning the index or
   `SIZE_MAX`. *Hint: linear scan, O(n).*
3. Move all zeroes to the end keeping order, in place, with read/write
   pointers. Compare with `std::stable_partition`.

Medium
4. Add `vec_reserve`-style **shrinking** to `c/02`: when `len < cap / 4`,
   halve `cap`. Why `/4` and not `/2`? *Hint: push/pop alternating at the
   boundary would reallocate every time (thrashing).*
5. Transpose an N×N matrix in place; then an R×C matrix stored in one block
   (out of place). Time row-major vs column-major writes.
6. Maximum sum of a **circular** subarray. *Hint: max(Kadane, total −
   min-subarray), careful when all are negative.*
7. Longest substring with at most 2 distinct characters, sliding window.

Hard
8. Give `cpp/01`'s `Vector<T>` an `insert(iterator, const T&)` with the strong
   exception guarantee. *Hint: what if the element being inserted is a
   reference into the vector itself and growth happens?*
9. Product of array except self, O(n), no division. *Hint: prefix products
   from the left times suffix products from the right.*
10. Maximum-sum rectangle in a 2D matrix in O(R²·C). *Hint: fix a pair of
    rows, collapse columns into a 1D array with prefix sums, run Kadane.*
11. Implement `memmove` yourself and test it against libc on all overlaps
    (then compare with `c/12`).

## 10. Remember forever

```
a[i] == *(a + i) at base + i*sizeof(T)    -> O(1) access, cache friendly
arrays DECAY to pointers when passed      -> always pass (ptr, n)
2D C arrays are row-major: base + (r*COLS + c); loop r outer, c inner
vector = {data, len, cap}; full -> cap*2, copy, free  -> amortised O(1)
  (1+2+4+...+n < 2n copies);  +k growth -> O(n^2)
insert/erase in middle = memmove the tail = O(n); swap-remove = O(1)
any reallocation invalidates ALL pointers/refs/iterators into a vector
two pointers / sliding window: each index moves <= n times -> O(n)
prefix: P[0]=0, sum(l..r) = P[r+1]-P[l];  Kadane: cur=max(x, cur+x)
rotate left k = rev(0,k) rev(k,n) rev(0,n)
C string = bytes + '\0'; strlen O(n); snprintf ret >= size => truncated
```

Review it on the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).
The strings card is in [notes/strings.md §10](notes/strings.md#10-remember-forever).

## 11. Self-test questions

1. Why is `a[i]` O(1) but finding the value `x` O(n)?
2. What does `sizeof(a)` give in `main` versus inside `void f(int a[])`, and why?
3. Write the address formula for `m[r][c]` in an `int m[R][C]`. Which
   dimension must a function parameter declare, and why?
4. Why is iterating a 2D array column-by-column slower even though it does
   the same number of operations?
5. Prove that doubling makes push_back amortised O(1). What goes wrong with
   `cap += 10`?
6. Why does a C++ vector use placement new instead of `new T[cap]`?
7. Why should a move constructor be `noexcept` if the type goes into a
   `std::vector`?
8. Name three operations that invalidate `std::vector` iterators.
9. In the sorted pair-sum, why is it safe to discard `a[L]` when the sum is
   too small?
10. When does the sliding-window technique stop working, and what do you use
    instead?
11. Derive `sum(l..r)` from a prefix array. Why have `P[0] = 0`?
12. Explain Kadane's decision at each index in one sentence.
13. Show the three-reversal rotation on `[1 2 3 4 5]` with k = 2.
14. What is the difference between `char s[] = "hi"` and `char *p = "hi"`?
15. Why does `strncpy` not make your copy safe on its own?
