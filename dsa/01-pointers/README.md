# 01 — Pointers: addresses, arithmetic, and every way they go wrong

> Every data structure in this repo is "some boxes plus some arrows".
> The arrows are pointers. Learn them once, properly, with pictures, and
> linked lists, trees, graphs, allocators and drivers all become easy.

---

## 1. Why this exists

Memory is one huge array of bytes, numbered from 0 upward. A **pointer** is
a variable that holds one of those numbers. That is the whole idea. Pointers
exist because:

* **Functions can only receive copies.** To let a function change your
  variable, you give it the *address* of the variable (`scanf("%d", &x)`).
* **Big things should not be copied.** Passing a 6 MB camera frame by value
  copies 6 MB; passing a pointer copies 8 bytes.
* **Structures link to each other.** A list node points to the next node,
  a tree node to its children. Without pointers there are no dynamic data
  structures.
* **Memory you did not declare** — heap blocks from `malloc`, a DMA buffer, an
  MMIO register at `0x0010_0000` — can only be reached through an address.
* **Code can be data.** Function pointers make callbacks, driver ops tables
  and state machines possible.

---

## 2. Mental model

**Analogy — a street of numbered lockers.** Every locker (byte) has a number
painted on it (its address). A variable is a locker (or a few adjacent ones)
with a name tag. A pointer is a locker whose *content is a locker number*,
written on a slip of paper. `&x` = "read the number painted on x's locker".
`*p` = "go to the locker whose number is written inside p".

```
  address   0x1000      0x1004      0x1008 ............. 0x1010
           ┌──────────┬──────────┐                     ┌──────────────────┐
  content  │    42    │          │                     │      0x1000      │
           └──────────┴──────────┘                     └──────────────────┘
  name         x                                              p
  type        int  (4 bytes)                              int * (8 bytes)

     &x  == 0x1000          the address of x
      p  == 0x1000          p STORES that address
     *p  == 42              follow the arrow: the int at 0x1000
     &p  == 0x1010          p has its own address too (it is a variable!)
```

Three numbers are always in play — **memorise this triangle**:

```
              &p ─── where the pointer lives
               │
               ▼
          ┌─────────┐
          │    p    │ ─── the value of p = an address (where it POINTS)
          └────┬────┘
               │  *
               ▼
          ┌─────────┐
          │   *p    │ ─── the object at that address
          └─────────┘
```

The **type** of a pointer tells the compiler two things about the target:
how many bytes `*p` reads, and how far `p + 1` jumps.

---

## 3. How it works

### 3.1 Addresses, `&` and `*`

| Expression | Read it as | Type (for `int x; int *p = &x;`) |
|---|---|---|
| `&x` | "address of x" | `int *` |
| `p` | "the address stored in p" | `int *` |
| `*p` | "the int at that address" | `int` (an lvalue: you can assign it) |
| `&p` | "address of the pointer itself" | `int **` |
| `*&x` | same as `x` | `int` |
| `&*p` | same as `p` | `int *` |

In a **declaration** `*` means "is a pointer"; in an **expression** it means
"dereference". `int *p, q;` declares one pointer and one **int** — the `*`
binds to the name, not the type. Write one declaration per line.

Little-endian (x86_64, the TDA4VM's A72 cores): the **lowest** byte of an int
sits at the **lowest** address. `01_address_and_deref.c` dumps the bytes of
`0x11223344` and you will see `44 33 22 11`.

### 3.2 Pointer arithmetic — why it scales by `sizeof`

`p + n` means "n *elements* further", i.e. the address `(char*)p + n*sizeof(*p)`.

```
 int32_t a[5];   int32_t *p = a;

 index      0         1         2         3         4         5
         ┌─────────┬─────────┬─────────┬─────────┬─────────┐
 a       │   10    │   20    │   30    │   40    │   50    │  (one past the end:
         └─────────┴─────────┴─────────┴─────────┴─────────┘   legal to POINT at,
 address 1000      1004      1008      1012      1016      1020  never to READ)
         p         p+1       p+2       p+3       p+4       p+5

 char  *c: c+1 = +1 byte      double *d: d+1 = +8 bytes
 int   *i: i+1 = +4 bytes     struct frame *f: f+1 = +sizeof(struct frame)
```

Rules:

* `a[i]` **is defined as** `*(a + i)`. So `i[a]` also works (please don't).
* `q - p` (same array) = number of **elements** between them, type `ptrdiff_t`.
* You may form pointers from `&a[0]` to `&a[N]` (one past the end). Anything
  else — even *computing* `a - 1` — is undefined.
* `*p++` is `*(p++)`: read, then move the pointer. `(*p)++` increments the
  value. `*++p` moves then reads.
* No arithmetic on `void *` in ISO C (gcc allows it as an extension, step 1);
  cast to `unsigned char *` for byte offsets.

### 3.3 Arrays vs pointers — decay and `sizeof`

**An array is not a pointer.** `int a[4]` is 16 bytes of ints and nothing
else; no address is stored anywhere. But in almost every expression the name
`a` **decays** to `&a[0]` (type `int *`).

```
   int arr[4];                          int *p = arr;
   ┌────┬────┬────┬────┐               ┌──────────┐
   │ 1  │ 2  │ 3  │ 4  │ ◄──────────── │ &arr[0]  │
   └────┴────┴────┴────┘               └──────────┘
   sizeof arr == 16                     sizeof p == 8

   arr, &arr[0], &arr  → SAME address, DIFFERENT types:
     arr + 1   → +4 bytes   (int *)
     &arr + 1  → +16 bytes  (int (*)[4], pointer to the whole array)
```

The three places an array does **not** decay: `sizeof arr`, `&arr`, and a
string literal initialising a char array (`char s[] = "hi"` copies the bytes).

**Function parameters**: `void f(int a[10])` is *rewritten* to `void f(int *a)`.
Inside `f`, `sizeof a` is 8. This is why every C API passes `(ptr, len)` and
why `ARRAY_LEN(a)` (`sizeof a / sizeof a[0]`) only works where the real array
is visible. gcc warns (`-Wsizeof-array-argument`) if you try.

| | array `int a[4]` | pointer `int *p` |
|---|---|---|
| storage | 4 ints | one address |
| `sizeof` | 16 | 8 |
| assignable (`a = ...`) | no | yes |
| `++` | no | yes |
| can be NULL | no | yes |

### 3.4 Pointer to pointer

You need `T **` when a function must **change the caller's pointer**:
allocate it, re-point it, or unlink a node.

```
   int **pp = &p;          int *p = malloc(...);       heap
   ┌──────────┐            ┌──────────┐                ┌─────┐
   │   &p     │ ─────────► │ 0x5020.. │ ─────────────► │  5  │
   └──────────┘            └──────────┘                └─────┘
      pp                    *pp  (== p)                 **pp
```

```c
void alloc_wrong(int *p)  { p = malloc(4); }   // changes a COPY → caller sees nothing, leak
int  alloc_right(int **pp){ *pp = malloc(4); } // writes the caller's pointer
```

The most elegant use — **removing from a singly linked list with no special
case for the head**: walk a pointer to the *link field* itself.

```
  head ─► [1|●]─► [2|●]─► [3|∅]
  ^link              link = &head, then &node1->next, then &node2->next ...
  when *link is the victim:   *link = victim->next;   free(victim);
  ─ if victim is the head, this rewrites `head`; otherwise prev->next. Same code.
```

`argv` is `char **`: a pointer to an array of `char *`, terminated by `NULL`.

### 3.5 `const` and pointers

Read **right to left from the name**:

```
 const int *p        p is a pointer to (int that is const)   → *p read-only, p movable
 int const *p        same thing
 int *const p        p is a const (pointer to int)           → p fixed, *p writable
 const int *const p  const pointer to const int              → nothing changes through p
```

Mnemonic: **const left of `*` protects the data; right of `*` protects the
pointer.** Functions that only read should take `const T *` — it documents
the contract (`memcpy(void *dst, const void *src, n)`) and lets callers pass
read-only data. Adding const is implicit; removing it needs a cast and a very
good reason. `char **` does *not* convert to `const char **` (it would open a
hole to write read-only data); `const char *const *` is fine.

### 3.6 `void *`

`void *` = an address with no element type. Any object pointer converts to
`void *` and back without a cast in C (C++ requires `static_cast`). You cannot
dereference it or do arithmetic on it. Generic C is built on `void *` plus a
**size**: `memcpy`, `qsort`, `bsearch`, `malloc`, and every "user data"
argument in callback APIs (`pthread_create`, GLib/GStreamer `gpointer`).

```c
void swap_any(void *a, void *b, size_t size);   // works on any type: caller gives the size
```

The price: the compiler cannot type-check. Pass the wrong size or cast to the
wrong type and you corrupt memory silently.

### 3.7 Function pointers and callbacks

A function's name decays to its address in `.text`. A function pointer stores
it; calling through it is an **indirect call** (`call *%rax`).

```c
int (*op)(int, int) = add;     // the parentheses matter!
int *op(int, int);             // ← this is a FUNCTION returning int*
typedef int (*binop_fn)(int, int);   // always typedef them
```

```
  dispatch table                      .text
  ┌──────────┬────────────┐          ┌─────────────────┐
  │ "double" │ ●──────────┼────────► │ cmd_double: ... │
  ├──────────┼────────────┤          ├─────────────────┤
  │ "negate" │ ●──────────┼────────► │ cmd_negate: ... │
  └──────────┴────────────┘          └─────────────────┘
```

Uses: `qsort`/`bsearch` comparators (return `(a>b)-(a<b)`, **not** `a-b`,
which overflows), command dispatch tables, state machines indexed by state,
interrupt vector tables, and **ops tables** — the C version of virtual
functions. The Linux kernel's `struct file_operations { .open, .read, .write,
.unlocked_ioctl ... }` is exactly the `struct device_ops` in
`07_function_pointers.c`.

C++ adds lambdas (can capture context; capture-less ones convert to plain
function pointers), `std::function` (type-erased, may allocate), and
templates taking a callable (inlinable — why `std::sort` is faster than
`qsort`). See `cpp/04_callbacks_modern.cpp`.

### 3.8 Pointers to structs, `->`, `offsetof`, `container_of`

`p->f` is exactly `(*p).f`. Each field sits at a fixed byte **offset** from
the start of the struct (`offsetof(type, field)`), so going *backwards* is
possible too:

```
 struct camera_frame                    p ──► +0   seq
 {                                            +4   width | height
     uint32_t seq;                            +8   node.next   ◄── &p->node
     uint16_t width, height;                  +16  node.prev
     struct list_head node;                   +24  timestamp_ns
     uint64_t timestamp_ns;
 };                     container_of(&p->node, struct camera_frame, node)
                          == (char *)&p->node - offsetof(struct camera_frame, node)
                          == p
```

That is how **intrusive lists** work in the Linux kernel: the list code only
knows `struct list_head`; `list_entry()`/`container_of()` recovers the
containing object. One node can sit on several lists through several hooks
with no extra allocation.

### 3.9 NULL, dangling and wild pointers

```
 NULL      p ──► ∅          on purpose "points nowhere". *p faults immediately (page 0
                            is never mapped). The BEST kind of failure: loud and early.

 dangling  p ──► [freed]    the object died: free()d, realloc()ed away, a local of a
                            function that returned, a vector that grew. Reads return
                            stale data, writes corrupt someone else's data. Silent.

 wild      p ──► ???        never initialised: holds stack garbage. Anything can happen.
```

Habits that prevent all three:

1. Initialise every pointer (`= NULL` or a real address).
2. `free(p); p = NULL;` together (a macro is fine). `free(NULL)` is a no-op.
3. Never return the address of a local; return by value, fill a caller buffer,
   or allocate and document who frees.
4. After `realloc` or a container growth, re-derive every interior pointer.
5. One clear **owner** per allocation. Copies of a pointer are *borrowers*.
6. Find the survivors with ASan (`-fsanitize=address`, on by default in this
   repo) — see [03-debugging](../03-debugging/README.md).

### 3.10 The `char *` string model

A C string = pointer to the first `char` of bytes that end with `'\0'`. The
length is **not stored**; `strlen` walks to the terminator in O(n).

```
 const char *lit = "cam";                     char arr[] = "cam";
 lit ──► .rodata: ['c']['a']['m']['\0']        arr (stack): ['c']['a']['m']['\0']
         READ-ONLY (writing = segfault)                     your own copy, writable
 sizeof lit == 8                               sizeof arr == 4 (terminator counts!)
```

Every string bug is one of: no room for the terminator, no terminator at all
(`strncpy` with a long source does **not** terminate), writing to a literal,
or reading past the end. Use `snprintf` (always terminates, returns the
length it *wanted*), check for truncation (`ret >= size`), and carry lengths
alongside pointers when you can.

### 3.11 Multidimensional arrays and pointers

`int a[3][4]` is **one contiguous block** of 12 ints, row-major:

```
 offset(bytes) 0   4   8   12  16  20  24  28  32  36  40  44
              [a00 a01 a02 a03|a10 a11 a12 a13|a20 a21 a22 a23]
               ^a[0]           ^a[1]           ^a[2]
 &a[r][c] == &a[0][0] + r*COLS + c
 a     decays to int (*)[4]   → a + 1 jumps 16 bytes (one row)
 a[1]  decays to int *        → a[1] + 1 jumps 4 bytes
```

`int **` is a *different shape*: an array of row pointers to separately
allocated rows (jagged, not contiguous, two loads per access). An
`int (*)[4]` cannot be passed where `int **` is expected.

Images and video frames are flat buffers with a **stride** (bytes per row,
often padded for hardware alignment): `pixel(x, y) = base[y * stride + x]`.
GStreamer's `GstVideoFrame`, V4L2 `bytesperline`, and the TI VPAC/DSS
drivers all use this.

### 3.12 `restrict` and strict aliasing

**Aliasing** = two pointers to the same bytes. If the compiler cannot rule it
out, it must reload from memory after every store through another pointer.

```c
void add_twice(int *dst, const int *src) { *dst += *src; *dst += *src; }
// add_twice(&a, &a) must give 4a, so *src is re-read after the first store.
void add_twice_restrict(int *restrict dst, const int *restrict src);
// you promise no overlap → the compiler may compute *dst += 2 * *src.
```

`memcpy` has `restrict` parameters (overlap = UB); `memmove` handles overlap.

**Strict aliasing rule**: an object may only be accessed through its own type
(or a compatible/qualified version, its signed/unsigned twin, or a
`char`/`unsigned char`). So `*(uint32_t *)&my_float` is UB: the optimiser may
assume the `float` and the `uint32_t` are different objects and reorder. Use
`memcpy` (compiles to a single move) or, in C only, a `union`. The Linux
kernel builds with `-fno-strict-aliasing` for historical reasons; your code
should not depend on that.

### 3.13 C++ references vs pointers

| | reference `T&` | pointer `T*` |
|---|---|---|
| can be null | no | yes |
| can be re-seated | no | yes |
| must be initialised | yes | no (but should be) |
| syntax | `r.f`, `r = v` | `p->f`, `*p = v` |
| has its own address/size | no (`&r == &x`) | yes (8 bytes) |
| arithmetic | no | yes |

Guideline: `const T&` for read-only "always there" parameters, `T&` for
must-modify parameters, `T*` for "optional" (nullptr = absent) or when you
re-point (iteration, relinking), by value for small types. A reference can
dangle too (reference to a vector element across `push_back`).

### 3.14 `nullptr`

In C++, `NULL` is an integer constant, so `f(NULL)` picks `f(int)` over
`f(char*)`, and templates forwarding `0` lose its "null pointer" meaning.
`nullptr` has its own type `std::nullptr_t`, converts to every pointer type
and to no integer. Always use it in C++.

### 3.15 Smart pointers preview

A raw pointer does not say who frees. Modern C++ puts ownership in the type:

```
 std::unique_ptr<T>   one owner, move-only, same size as T*, deletes in its destructor
 std::shared_ptr<T>   shared ownership, atomic refcount in a control block
 std::weak_ptr<T>     non-owning observer of a shared_ptr; lock() to use; breaks cycles
 T*, T&               non-owning "borrow" — totally fine for parameters
```

Full details, the reference-cycle leak, and the rule of 0/3/5 are in
[02-memory-management](../02-memory-management/README.md).

---

## 4. Complexity table

| Operation | Time | Why |
|---|---|---|
| `&x`, `p + n`, `p - q`, `p == q` | O(1) | integer add/sub/compare on addresses |
| `*p`, `p->f`, `a[i]` | O(1) | one memory load (cost depends on caches: 1 ns L1 … 100 ns DRAM) |
| `p->a->b->c` | O(k) loads | each `->` is a dependent load (pointer chasing) |
| `strlen(s)` | O(n) | length not stored; must find `'\0'` |
| function-pointer call | O(1) | indirect branch; may mispredict, cannot be inlined |
| `swap_any(a, b, size)` | O(size) | byte copy |
| Space of a pointer | 8 B (LP64), 4 B (ILP32) | regardless of what it points to |

---

## 5. C vs C++

| Need | C | C++ |
|---|---|---|
| alias a parameter | `T *` | `T &` / `const T &` |
| optional parameter | `T *` (NULL) | `T *` (nullptr) or `std::optional<T>` |
| null | `NULL` | `nullptr` |
| generic code | `void *` + size | templates |
| callback | fn ptr + `void *user` | lambda, `std::function`, template param |
| owning pointer | `T *` + comment "caller frees" | `std::unique_ptr<T>` |
| shared ownership | manual refcount (`kref`) | `std::shared_ptr<T>` |
| array view | `(T *, size_t)` | `std::span<T>` (C++20), iterators |
| cast | `(T *)p` | `static_cast` / `reinterpret_cast` |
| `void *` → `T *` | implicit | needs `static_cast` |

---

## 6. Where it is used in the real world

* **Linux kernel**: `struct list_head` + `container_of` everywhere;
  `struct file_operations`, `struct platform_driver` ops tables; `__iomem`
  pointers to MMIO (`ioremap` returns one; you access it with `readl`/`writel`);
  `copy_from_user` because a user pointer is not a kernel pointer; `kref` for
  refcounting.
* **Device drivers on the TDA4VM**: DMA descriptors hold *physical/bus*
  addresses, the CPU uses *virtual* ones — two different pointer worlds for
  the same buffer (`dma_addr_t` vs `void *`).
* **GStreamer / GLib**: `gpointer user_data` in every callback, `GstBuffer`
  maps returning `guint8 *data` + `gsize size`, refcounted objects
  (`gst_object_ref/unref`), `GList` intrusive-ish lists.
* **Databases**: B-tree pages linked by page numbers (pointers that survive on disk).
* **Compilers/interpreters**: ASTs, symbol tables, dispatch tables for opcodes.
* **Games/embedded**: function-pointer state machines, ISR vector tables.

---

## 7. Common mistakes and pitfalls

1. `int *p, q;` — `q` is an `int`.
2. Dereferencing NULL, uninitialised (wild) or freed (dangling) pointers.
3. Returning the address of a local variable.
4. `sizeof(p)` instead of `sizeof(*p)` in `malloc`; `sizeof(arr)` on an array parameter.
5. Off-by-one: reading `a[N]`; forgetting the `'\0'` byte in string buffers.
6. Keeping pointers/iterators into a buffer across `realloc` / `vector::push_back`.
7. Comparator `return a - b;` (overflow) in `qsort`.
8. Casting away `const` and writing (string literals live in read-only memory).
9. Type punning with `*(uint32_t *)&f` (strict aliasing UB) — use `memcpy`.
10. Lying with `restrict` (overlapping arguments).
11. Pointer arithmetic outside `[&a[0], &a[N]]`, or on `void *` (non-portable).
12. Mismatched declarations across files: `extern int *x;` vs `int x[10];`.
13. Two owners both calling `free` (double free) — or none (leak).

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_address_and_deref.c](c/01_address_and_deref.c) | `&`, `*`, byte dump, endianness, output parameters | `make run P=dsa/01-pointers/c/01_address_and_deref.c` |
| [c/02_pointer_arithmetic.c](c/02_pointer_arithmetic.c) | `p+1` scaling, `a[i]==*(a+i)`, `*p++` vs `(*p)++`, one-past-the-end | `make run P=dsa/01-pointers/c/02_pointer_arithmetic.c` |
| [c/03_arrays_vs_pointers.c](c/03_arrays_vs_pointers.c) | decay, `sizeof` differences, `&arr+1`, array parameters | `make run P=dsa/01-pointers/c/03_arrays_vs_pointers.c` |
| [c/04_pointer_to_pointer.c](c/04_pointer_to_pointer.c) | `T**` out-params, `argv`, linked-list removal via pointer-to-link | `make run P=dsa/01-pointers/c/04_pointer_to_pointer.c` |
| [c/05_const_pointers.c](c/05_const_pointers.c) | `const int *` vs `int *const` vs both | `make run P=dsa/01-pointers/c/05_const_pointers.c` |
| [c/06_void_pointers.c](c/06_void_pointers.c) | generic swap/find/for_each with `void *` + size | `make run P=dsa/01-pointers/c/06_void_pointers.c` |
| [c/07_function_pointers.c](c/07_function_pointers.c) | qsort/bsearch, dispatch table, driver ops table, state machine | `make run P=dsa/01-pointers/c/07_function_pointers.c` |
| [c/08_struct_pointers.c](c/08_struct_pointers.c) | `->`, `offsetof`, `container_of`, intrusive list | `make run P=dsa/01-pointers/c/08_struct_pointers.c` |
| [c/09_null_dangling_wild.c](c/09_null_dangling_wild.c) | the three bad pointers and the habits that prevent them | `make run P=dsa/01-pointers/c/09_null_dangling_wild.c` |
| [c/10_strings_char_pointers.c](c/10_strings_char_pointers.c) | literal vs array, strlen/strlcpy/strcmp by hand, truncation | `make run P=dsa/01-pointers/c/10_strings_char_pointers.c` |
| [c/11_multidim_arrays.c](c/11_multidim_arrays.c) | `int[3][4]`, `int (*)[4]`, `int **`, strided image buffer | `make run P=dsa/01-pointers/c/11_multidim_arrays.c` |
| [c/12_restrict_and_aliasing.c](c/12_restrict_and_aliasing.c) | aliasing, `restrict`, `memmove`, type punning done right | `make run P=dsa/01-pointers/c/12_restrict_and_aliasing.c` |
| [cpp/01_references_vs_pointers.cpp](cpp/01_references_vs_pointers.cpp) | when to use `T&`, `const T&`, `T*`; reference to pointer | `make run P=dsa/01-pointers/cpp/01_references_vs_pointers.cpp` |
| [cpp/02_nullptr.cpp](cpp/02_nullptr.cpp) | why `nullptr` beats `NULL`/`0` with overloads and templates | `make run P=dsa/01-pointers/cpp/02_nullptr.cpp` |
| [cpp/03_smart_pointers_preview.cpp](cpp/03_smart_pointers_preview.cpp) | `unique_ptr`, `shared_ptr`, `weak_ptr` in 60 lines | `make run P=dsa/01-pointers/cpp/03_smart_pointers_preview.cpp` |
| [cpp/04_callbacks_modern.cpp](cpp/04_callbacks_modern.cpp) | fn ptr vs lambda vs `std::function` vs template; member pointers | `make run P=dsa/01-pointers/cpp/04_callbacks_modern.cpp` |
| [cpp/05_iterators_are_pointers.cpp](cpp/05_iterators_are_pointers.cpp) | raw pointers as STL iterators, a span-like view, invalidation | `make run P=dsa/01-pointers/cpp/05_iterators_are_pointers.cpp` |

---

## 9. Exercises

1. **(easy)** Draw (on paper) the boxes and arrows for
   `int x = 5; int *p = &x; int **pp = &p;` with made-up addresses. Then
   compute `*pp`, `**pp`, `&*p`, `*&pp`.
2. **(easy)** Write `void swap(int *a, int *b)` and explain why `void swap(int a, int b)` cannot work.
3. **(easy)** Without running it: what is `sizeof` of `char s[] = "hello"`,
   `char *t = "hello"`, `int m[2][3]`, `m[0]`, `&m`?
4. **(medium)** Implement `my_memmove(void *dst, const void *src, size_t n)`
   that handles overlap. *Hint:* copy backwards when `dst > src`.
5. **(medium)** Write a generic `void *my_bsearch(...)` with the same signature
   as `bsearch` and test it with ints and with structs.
6. **(medium)** Turn `04_pointer_to_pointer.c`'s `remove_value` into
   `insert_sorted(struct node **head, int v)` using the same pointer-to-link trick.
7. **(medium)** Write a dispatch table for a tiny calculator: `"add 2 3"` style
   commands parsed from a fixed array of strings (no stdin).
8. **(hard)** Implement a 2-D matrix multiply on flat buffers with explicit
   strides, then show (with `restrict`) the speedup at `-O2`.
9. **(hard)** Implement a doubly linked intrusive list (`list_add`, `list_del`,
   `list_for_each_entry` macro) like the kernel's, and put one object on two lists.
10. **(hard)** Read the declaration `void (*signal(int sig, void (*func)(int)))(int);`
    out loud. Rewrite it with a typedef.

---

## 10. Remember forever

```
pointer = variable holding an address.  &x = address of x.  *p = object at p.
Type of p decides: how many bytes *p reads, and how far p+1 jumps (sizeof *p).
a[i] == *(a+i).  Arrays DECAY to &a[0] except in sizeof, &, and string init.
Array parameter = pointer: sizeof is 8 → always pass (ptr, len).
To change the caller's T, pass T*.  To change the caller's T*, pass T**.
const left of * → data read-only; right of * → pointer fixed.
void* = address without type: no deref, no arithmetic, pass a size.
NULL faults loudly; dangling/wild corrupt silently → init, free+NULL, one owner, ASan.
String = char* to bytes ending in '\0'; length not stored; budget the terminator.
container_of(ptr, type, member) = (type*)((char*)ptr - offsetof(type, member)).
```

---

## 11. Self-test questions

1. What three different numbers are involved in `int *p = &x;`? Which one does `printf("%p", p)` print?
2. Why does `p + 1` add 4 for `int *` and 8 for `double *`?
3. Give the three cases where an array name does not decay to a pointer.
4. Why is `sizeof(a)` 8 inside `void f(int a[100])`?
5. When do you need `T **`? Explain the "pointer to the link" list removal.
6. Read aloud: `const char *const *p`. What can and cannot change?
7. Why can't you dereference a `void *`? How do `qsort` and `memcpy` cope?
8. Write the declaration of a pointer to a function taking `(const void *, const void *)` and returning `int`.
9. How does `container_of` work, and why do kernel lists not need one allocation per node?
10. NULL vs dangling vs wild: which one fails loudly and why?
11. Why does `strncpy` not protect you, and what do you use instead?
12. What is the difference between `int a[3][4]`, `int (*p)[4]`, and `int **pp` in memory?
13. What does `restrict` promise? What is the strict aliasing rule and how do you read a float's bits legally?
14. When would you choose `T&`, `const T&`, `T*`, `unique_ptr<T>` for a C++ parameter?

Review schedule: see [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).
