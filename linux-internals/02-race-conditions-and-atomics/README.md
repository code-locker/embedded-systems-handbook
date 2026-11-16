# 02 — Race Conditions and Atomics

> The moment two threads touch the same variable and at least one of them
> writes, you need a rule for *who sees what, when*. Atomics are the
> lowest-level rule the hardware gives you: an operation that cannot be
> split, plus a **memory order** that says which other reads and writes may
> or may not move around it. Every mutex, condition variable, lock-free
> queue and RCU in later lessons is built from these few primitives.

---

## 1. Why this exists

Lesson [01](../01-processes-and-threads/README.md) showed that threads
share memory. This lesson shows why sharing is hard:

- `counter++` from four threads loses *more than half* of the increments
  (`c/01_racy_counter_bug.c` — run it).
- A flag that says "data ready" can be seen **before** the data it guards,
  on the BeagleBone's Cortex-A72 cores and, through compiler reordering, on
  any CPU.
- Two threads that each set "I want in" and then check the other's flag can
  *both* get in — even on x86.

Atomics fix the first problem (**atomicity**) and, with the right memory
order, the other two (**visibility** and **ordering**). Without them, the C
and C++ standards say a **data race is undefined behaviour**: the compiler
is allowed to assume it never happens, so "it works on my PC" means nothing.

---

## 2. Mental model

**Each core has a private notebook (store buffer + cache) and there is one
shared whiteboard (memory).** A core writes into its notebook first and
copies it to the whiteboard *later*, not necessarily in the order it wrote.
It may also glance at the whiteboard *early*, before finishing earlier
work. Atomics + memory orders are the rules: "copy everything in my notebook
to the board before this line" (**release**), "don't read the board ahead
of this line" (**acquire**), "everyone agrees on one order of these lines"
(**seq_cst**).

```
       core 0 (A72)                        core 1 (A72)
   +------------------+                +------------------+
   | registers        |                | registers        |
   | store buffer  ---+--- drains ---+ | store buffer     |
   |  [data=42]       |   (any order | |  [ ]             |
   |  [flag=1 ]       |    on ARM)   | |                  |
   | L1 cache         |              | | L1 cache         |
   +--------+---------+              | +--------+---------+
            |    cache-coherence protocol (MESI): one writer   |
            +-------------- per 64-byte line at a time --------+
                                    |
                           L2 / DDR (memory)
```

Three separate properties — keep them apart:

| Property      | Question it answers                                      | Broken example |
|---------------|----------------------------------------------------------|----------------|
| **Atomicity** | Can another thread see/act in the *middle* of my operation? | `counter++` loses updates |
| **Visibility**| Will another thread *ever* see my write?                 | loop on a plain `bool stop` never exits |
| **Ordering**  | Does another thread see my writes in the order I made them? | sees `ready == 1` but stale `data` |

---

## 3. How it works

### 3.1 Data race vs race condition

- **Data race** (a language term): two threads access the same memory
  location concurrently, at least one writes, and at least one access is
  **not atomic**, with no happens-before between them. Undefined behaviour.
  ThreadSanitizer finds these.
- **Race condition** (a logic term): the result depends on timing. You can
  have one *without* a data race: every access atomic, but the *sequence* is
  wrong.

```
  data race, no "logic" problem     race condition, no data race
  -----------------------------     -----------------------------------------
  int x;  T1: x = 1;  T2: r = x;    if (atomic_load(&slots) < LIMIT)   // check
                                        atomic_fetch_add(&slots, 1);    // act
  UB even if "any value is fine"    two threads both pass the check: LIMIT+1
  TSan: reports it                  TSan: silent. Fix: one CAS (c/02, [3])
```

### 3.2 Why `counter++` is not atomic  (`c/01_racy_counter_bug.c`)

`counter++` compiles to three steps, and on ARM there is no way to do it
in one ordinary instruction:

```
  x86-64:  mov eax,[counter] ; add eax,1 ; mov [counter],eax
  arm64 :  ldr w1,[x0]       ; add w1,w1,#1 ; str w1,[x0]

     thread A              thread B              counter
     LOAD  r=5                                      5
                           LOAD  r=5                5
     ADD   r=6                                      5
                           ADD   r=6                5
     STORE 6                                        6
                           STORE 6                  6     <- one increment lost
```

`make run P=linux-internals/02-race-conditions-and-atomics/c/01_racy_counter_bug.c SAN=tsan`
prints `WARNING: ThreadSanitizer: data race` with both stacks. The
ASan build just prints how many increments were lost — a different number
each run.

### 3.3 `volatile` is NOT synchronisation

`volatile` tells the compiler "this memory may change behind your back (a
hardware register), so do every access exactly as written". That is all:

| Needed for threads | `volatile` gives it? |
|--------------------|----------------------|
| atomic RMW (`++`)  | **no** — still load/add/store (`c/01` loses updates on the volatile counter too) |
| ordering with *other* variables | **no** — the CPU may still reorder; the compiler may move non-volatile accesses across it |
| no data race (defined behaviour) | **no** — still a data race in C11/C++11 |

Use `volatile` for memory-mapped I/O registers (with the kernel's
`readl`/`writel`, which add barriers), and `setjmp`/signal-handler corner
cases. For threads use atomics or locks. (MSVC once gave `volatile`
acquire/release meaning; GCC/Clang never did, and ARM certainly does not.)

### 3.4 C11 `<stdatomic.h>`  (`c/02_atomic_counter_and_cas.c`)

```c
#include <stdatomic.h>
atomic_int  counter;                  /* = _Atomic int; static => zero-initialised */
_Atomic(uint64_t) top;
atomic_init(&x, 0);                   /* non-atomic init of a fresh object */

atomic_load(&x)        atomic_store(&x, v)        atomic_exchange(&x, v)
atomic_fetch_add/sub/or/and/xor(&x, v)   /* return the OLD value */
atomic_compare_exchange_strong(&x, &expected, desired)
atomic_compare_exchange_weak  (&x, &expected, desired)
atomic_flag f = ATOMIC_FLAG_INIT; atomic_flag_test_and_set(&f); atomic_flag_clear(&f);
atomic_thread_fence(order);   atomic_is_lock_free(&x);
/* every function has an _explicit(..., memory_order) form; the plain one is seq_cst */
```

Each atomic is still a normal variable in memory; the type tells the
compiler to use special instructions and not to tear, cache or reorder it
beyond what the memory order allows. `atomic_flag` is the only type
guaranteed lock-free; for others ask `atomic_is_lock_free` (8-byte and
smaller are lock-free on x86-64 and arm64; 16-byte types go through
libatomic, which may use a lock).

### 3.5 Compare-and-swap loops, weak vs strong, `atomic_flag`

When the new value depends on the old one in a way no `fetch_*` covers
(max, bounded increment, push onto a list), use the CAS loop:

```
  old = load(x)
  do {
      new = f(old)
  } while (!compare_exchange_weak(&x, &old, new));
           \__ x == old ? (x = new, true) : (old = x, false)   one atomic step
```

- On failure the CAS **writes the current value into `old`** — do not reload.
- `_weak` may fail **spuriously** (value matched but the store failed). On
  ARMv8.0 a CAS is an `ldxr`/`stxr` pair, and `stxr` fails if the core lost
  its exclusive monitor (interrupt, cache eviction). In a retry loop that is
  harmless and saves an inner loop, so: **weak in loops, strong for a
  one-shot test**.
- The two memory-order arguments are for success (an RMW) and failure (just
  a load).
- `atomic_flag` + `test_and_set(acquire)` / `clear(release)` is the
  smallest possible spinlock (`c/02` [4], `cpp/01` [5]). Lesson
  [06](../06-spinlocks-and-futex/README.md) makes a real one and explains
  why user space should usually block in a futex instead of spinning.

### 3.6 Memory orders

| Order | Applies to | Guarantees | Typical use |
|-------|------------|------------|-------------|
| `relaxed` | any | atomicity + a single order *per variable*; nothing about other variables | counters, statistics, stop flags, IDs |
| `release` | stores (and RMW) | no earlier read/write moves **after** it | publish: "data is ready" |
| `acquire` | loads (and RMW) | no later read/write moves **before** it | subscribe: "is data ready?" |
| `acq_rel` | RMW | both | CAS in lock-free structures, refcount drop |
| `seq_cst` | any | acquire/release + **one global order** of all seq_cst ops that every thread agrees on | default; Dekker/Peterson; when in doubt |
| `consume` | loads | dependency ordering; every compiler treats it as acquire | don't use |

```
           release store                         acquire load
  ...writes/reads...                       ─────────────────────────
  ──── nothing below may move up? no:      load_acquire(flag)
       nothing ABOVE may move DOWN ────    ──── nothing BELOW may move UP
  store_release(flag)                       ...writes/reads...
```

When a thread's **acquire load reads the value written by another thread's
release store**, the two *synchronise*: everything before the release
**happens-before** everything after the acquire. That is the only way
non-atomic data becomes safely visible between threads — and it is exactly
what `pthread_mutex_unlock` (release) / `pthread_mutex_lock` (acquire) do
for you (lesson [03](../03-mutexes-and-deadlock/README.md)).

### 3.7 Litmus test 1: message passing  (`c/03_memory_order_litmus.c` [1])

```
  data = 42;                 (W1)     while (!flag) ;            (R1)
  flag = 1;                  (W2)     assert(data == 42);        (R2)
```

| Orders used | Can R2 see the old data? |
|-------------|---------------------------|
| all relaxed | **yes** on ARM (W1/W2 drain out of order, or R2 is speculated before R1); on x86 the hardware keeps store→store and load→load order, but the *compiler* may still reorder |
| flag: release store + acquire load | **never** |
| relaxed + `atomic_thread_fence(release)` before W2, `fence(acquire)` after R1 | **never** |

The program runs 20 000 rounds of each and CHECKs that the
release/acquire and fence versions see zero anomalies. The relaxed count is
printed: expect 0 on x86 and try it on the board.

### 3.8 Litmus test 2: store buffering / Dekker  (`c/03` [2])

```
  x = y = 0
  T1: x = 1;  r1 = y;          T2: y = 1;  r2 = x;
  Can r1 == 0 && r2 == 0 ?
```

Intuition says no; real CPUs say **yes, even x86**, because each core's
store sits in its store buffer while its load reads the old value from cache
(store→load reordering is the one reordering x86-TSO allows).
Release/acquire does **not** forbid it (the store and the load are to
*different* variables, and release/acquire only orders in one direction).
Only `seq_cst` on both stores and loads forbids it. The program prints how
often `0/0` happened with relaxed and with release/acquire (from tens to
thousands of times per 20 000 rounds on a desktop x86, depending on build
and load) and CHECKs it is 0 with seq_cst. This is why Peterson's/Dekker's mutual exclusion needs seq_cst or
a full fence, and why Linux has `smp_mb()` in addition to
acquire/release.

### 3.9 Fences, compiler vs CPU reordering, ARM vs x86

Two different things reorder your code:

1. **The compiler**: moves, merges, hoists loads out of loops, deletes
   "dead" stores — anything a single thread cannot notice. A compiler-only
   barrier: `atomic_signal_fence()` / `asm volatile("" ::: "memory")`
   (the kernel's `barrier()`).
2. **The CPU**: out-of-order execution, store buffers, speculative loads,
   non-FIFO cache coherence. Prevented by special instructions.

Atomics with a memory order handle **both**: they constrain the compiler
*and* emit the right instructions.

`atomic_thread_fence(order)` is an order without a variable: a release
fence orders all earlier accesses before all *later stores*; an acquire
fence orders all *earlier loads* before all later accesses. Useful to
publish several relaxed flags at once or to upgrade a relaxed load after
you know you need it. (ThreadSanitizer does not model fences, so `c/03`
keeps its fence-version payload atomic.)

What the hardware does:

| C11 operation | x86-64 (TSO: only store→load reordering) | ARMv8 / Cortex-A72 (weak: everything may reorder) |
|---------------|-------------------------------------------|---------------------------------------------------|
| load relaxed  | `mov`                                     | `ldr`                                             |
| load acquire  | `mov` (free)                              | `ldar` (ARMv8.3: `ldapr`)                         |
| store relaxed | `mov`                                     | `str`                                             |
| store release | `mov` (free)                              | `stlr`                                            |
| store seq_cst | `xchg` (or `mov` + `mfence`)              | `stlr` (and `ldar` is never reordered before an earlier `stlr`) |
| fetch_add     | `lock xadd`                               | ARMv8.0: `ldaxr`/`add`/`stlxr`/`cbnz` loop; ARMv8.1 LSE: `ldaddal` (the A72 is ARMv8.0: no LSE) |
| CAS           | `lock cmpxchg`                            | ARMv8.0: `ldaxr`/`cmp`/`stlxr` loop; LSE: `casal` |
| fence acq/rel | nothing (compiler barrier)                | `dmb ishld` / `dmb ish`                           |
| fence seq_cst | `mfence`                                  | `dmb ish`                                         |

The practical consequence: **code that is wrong but "works" on x86 can
fail on the BeagleBone**, because x86 gives acquire/release for free on
every plain load and store. Test concurrent code under TSan *and* on the
board. To see the instructions, cross-compile and read the disassembly
(`make arm` writes a `.dis` next to every arm-architecture/ program).

### 3.10 The ABA problem  (`c/04_treiber_stack_aba.c`)

A **Treiber stack** is a lock-free linked stack: push and pop each read
`top`, prepare the new value, and CAS it in. CAS compares *values*, so it
cannot tell "unchanged" from "changed and changed back":

```
  start:          top -> A -> B -> C
  T1 pop starts:  reads top=A, A.next=B      ... preempted
  T2: pop A, pop B, push A                   top -> A -> C   (T2 owns B)
  T1 resumes:     CAS(top, A, B) SUCCEEDS    top -> B -> C   <- B is in use
                                                              by T2, A is lost
```

Fixes:

| Fix | How | Cost |
|-----|-----|------|
| **Tag / version counter** | CAS `{pointer, counter}` together; bump the counter on every change | needs a double-width CAS, or pack an index + tag in 64 bits (what `c/04` does with a node pool) |
| Never reuse a node while anyone may hold it | hazard pointers, epoch-based reclamation, RCU (lesson [07](../07-rwlocks-seqlocks-and-rcu/README.md)) | more machinery, but also solves use-after-free |
| LL/SC directly | ARM `ldxr`/`stxr` fails if the line was *written*, so it is ABA-immune — but C/C++ do not expose LL/SC, only CAS | not portable |
| Use a lock | a mutex has no ABA | may be fast enough — measure first |

`c/04` replays the interleaving deterministically (untagged CAS succeeds
and corrupts; tagged CAS fails), then stress-tests the tagged stack with 4
threads and checks that every node is back exactly once.

### 3.11 False sharing  (`c/05_false_sharing.c`)

Coherence works per **cache line** (64 bytes on Cortex-A72 and most x86).
Two threads writing *different* variables in the same line force the line
to bounce between cores on every write:

```
   line: [ a | b | ....... ]       core0 writes a -> invalidates core1's copy
          ^c0  ^c1                 core1 writes b -> invalidates core0's copy
```

`c/05` times two threads incrementing two adjacent counters vs two
counters padded to separate lines (`_Alignas(64)`, C++
`alignas(64)` or `std::hardware_destructive_interference_size` from C++17
where the library provides it). Typical result: 3–4x faster with padding,
and much faster again if each thread sums locally and writes once. Some x86
parts prefetch line *pairs*, so high-performance code sometimes pads to 128.
Timings are printed, never asserted.

### 3.12 Putting it together: the lock-free SPSC ring

The single-producer/single-consumer ring in
[dsa/07-stacks-and-queues, section 3.8](../../dsa/07-stacks-and-queues/README.md)
(`c/08_spsc_lockfree_ring.c`) uses everything here: each index has one
writer (no RMW needed), the producer's `tail` store is **release**, the
consumer's `tail` load is **acquire** (message passing, 3.7), and `head`
and `tail` are on separate cache lines (3.11). Read it now — it is the
most useful lock-free structure in embedded Linux. The blocking
mutex+condvar version is in
[dsa/19-system-design-structures, c/03](../../dsa/19-system-design-structures/c/03_ring_buffer_producer_consumer.c).

---

## 4. Complexity table

| Operation (uncontended, same core keeps the line) | Approx. cost | Why |
|---------------------------------------------------|--------------|-----|
| relaxed load / store                              | ~1 ns, same as plain | ordinary `ldr`/`str` / `mov` |
| acquire load / release store                      | ~1 ns (x86 free; ARM `ldar`/`stlr` slightly more) | limits reordering around it |
| seq_cst store                                     | ~10–30 ns on x86 (`xchg` drains store buffer) | waits for the store buffer |
| `fetch_add` / CAS, uncontended                    | ~5–20 ns     | exclusive line ownership + ordering |
| `fetch_add` / CAS, contended across cores         | ~50–200+ ns  | the line migrates between cores each time |
| CAS loop under heavy contention                   | unbounded retries per thread (lock-free, not wait-free) | someone always makes progress, maybe not you |
| false sharing                                     | turns ~1 ns stores into ~50 ns line transfers | coherence works per line |
| Treiber push/pop                                  | O(1) per attempt, retried on conflict | one CAS on `top` |

Lock-free means *system-wide progress* (some thread always completes);
wait-free means *every* thread completes in bounded steps (e.g. the SPSC
ring). Neither means "faster than a mutex" — measure.

---

## 5. C vs C++

| Concept | C11 `<stdatomic.h>` | C++17 `<atomic>` |
|---------|---------------------|------------------|
| type | `_Atomic int`, `atomic_int` | `std::atomic<int>` |
| load / store | `atomic_load(&a)`, `atomic_store(&a, v)` | `a.load()`, `a.store(v)`, or `a` / `a = v` (seq_cst) |
| RMW | `atomic_fetch_add(&a, 1)` | `a.fetch_add(1)`, `++a`, `a += 1` (all one atomic RMW) |
| **trap** | `a = a + 1` on `_Atomic` = load + store | `a = a + 1` = load + store: lost updates (`cpp/01` [1]) |
| CAS | `atomic_compare_exchange_weak(&a, &exp, des)` | `a.compare_exchange_weak(exp, des)` |
| explicit order | `atomic_*_explicit(..., memory_order_acquire)` | `a.load(std::memory_order_acquire)` |
| flag | `atomic_flag`, `ATOMIC_FLAG_INIT` | `std::atomic_flag` (C++20: `test()`, `wait()`) |
| lock-free query | `atomic_is_lock_free(&a)` (run time), `ATOMIC_INT_LOCK_FREE` | `a.is_lock_free()`, `std::atomic<T>::is_always_lock_free` (compile time) |
| fence | `atomic_thread_fence(o)` | `std::atomic_thread_fence(o)` |
| user types | none | `std::atomic<T>` for any trivially copyable `T` (may hide a lock) |
| atomic view of a plain object | — | C++20 `std::atomic_ref<T>` (mention only) |
| block until changed | futex syscall | C++20 `a.wait(old)` / `a.notify_one()` |
| smart pointer | — | C++20 `std::atomic<std::shared_ptr<T>>` |

---

## 6. Where it is used in the real world

- **Linux kernel**: `atomic_t`/`atomic64_t` (`atomic_inc`,
  `atomic_cmpxchg`), `refcount_t` (saturating refcounts: the
  `fetch_update` pattern from `cpp/01`), `READ_ONCE`/`WRITE_ONCE`
  (relaxed-like, stops the compiler tearing/merging), `smp_load_acquire` /
  `smp_store_release`, `smp_mb`/`smp_rmb`/`smp_wmb` (→ `dmb ish`, `dmb
  ishld`, `dmb ishst` on arm64), and `llist.h` — a lock-free Treiber-style
  list used for deferred work. Documented in
  `Documentation/memory-barriers.txt` and modelled by the LKMM
  (`tools/memory-model`). Kernel side of this track:
  [linux-device-drivers/01-kernel-synchronization](../../linux-device-drivers/01-kernel-synchronization/README.md).
- **glibc**: the `pthread_mutex` fast path is one CAS on a futex word;
  `pthread_once` and `std::call_once` use an acquire load for the "already
  done" check (lesson [08](../08-barriers-once-and-thread-pools/README.md)).
- **GStreamer**: `GstMiniObject` refcounts use `g_atomic_int_inc` /
  `g_atomic_int_dec_and_test`; `GstAtomicQueue` is a lock-free queue;
  pad/element flags use `g_atomic_int_*`
  ([gstreamer/08-queues-threads-tee](../../gstreamer/08-queues-threads-tee/README.md)).
- **C++ standard library**: `std::shared_ptr` control block counts
  (`fetch_add` relaxed to increment, `fetch_sub` acq_rel to decrement).
- **Embedded / RTOS**: on Cortex-M (FreeRTOS) the same ideas appear as
  `LDREX`/`STREX` and `__DMB()`; single-core RTOSes often "make it atomic"
  by disabling interrupts (`taskENTER_CRITICAL()`), which does **not**
  work across the TDA4VM's two A72 cores or between the A72 and the R5F.
  Inter-core mailboxes and shared-memory rings between Linux and the R5F
  use exactly the release/acquire publish pattern plus cache maintenance.
- **DPDK, Folly, liburcu, Linux `io_uring`**: lock-free rings whose
  head/tail use acquire/release and sit on separate cache lines.

---

## 7. Common mistakes and pitfalls

1. **"It's just an int, the CPU writes it in one go"** — the *increment*
   still is three steps, and a data race is UB regardless of the CPU.
2. **`volatile` for thread communication** — not atomic, not ordered, still
   a data race (`c/01`).
3. **`a = a + 1` on an atomic** — two atomic ops, not one (`cpp/01` [1]).
   Use `++a` / `fetch_add`.
4. **Check-then-act on atomics** — each step atomic, the sequence not
   (`c/02` [3]). Use one CAS, or a lock.
5. **Relaxed where ordering matters** — fine for a counter, wrong for a
   "ready" flag that publishes data. Works on x86, fails on ARM.
6. **Believing acquire/release forbids store→load reordering** — it does not
   (3.8). Dekker-style "set mine, read yours" needs seq_cst.
7. **Passing `memory_order` through a function parameter** — GCC sees a
   non-constant order and silently uses seq_cst. Correct but slower, and it
   hid every reordering in the first version of `c/03`. Use constants
   (macros/templates).
8. **Reloading `expected` after a failed CAS** — unnecessary; the CAS
   already wrote the current value into it. Forgetting that it was
   overwritten is the opposite bug.
9. **ABA in lock-free structures with recycled nodes** (`c/04`); and
   use-after-free when nodes are `free`d while another thread may still read
   them.
10. **False sharing** of per-thread counters, or of a lock and the data it
    protects with an unrelated hot variable (`c/05`).
11. **16-byte `std::atomic<T>`** silently uses libatomic (maybe a lock) — not
    usable from a signal handler or across processes in shared memory. Check
    `is_always_lock_free`.
12. **Testing only on x86 / only with -O0** — test on the board, with
    `OPT=1`, and with `make run P=... SAN=tsan`. TSan does not understand
    `atomic_thread_fence`, so fence-based code may show false reports.
13. **Hand-rolled lock-free code where a mutex would do** — harder to get
    right, often not faster under real contention.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_racy_counter_bug.c](c/01_racy_counter_bug.c) | **Intentional bug**: `counter++` on a plain and a `volatile` int from 4 threads. TSan reports the data races; the ASan build prints the lost updates | `make run P=linux-internals/02-race-conditions-and-atomics/c/01_racy_counter_bug.c SAN=tsan` |
| [c/02_atomic_counter_and_cas.c](c/02_atomic_counter_and_cas.c) | `atomic_fetch_add`, CAS-loop atomic max, check-then-act race condition vs CAS fix, `atomic_flag` spinlock, exchange / fetch_or / strong CAS | `make run P=linux-internals/02-race-conditions-and-atomics/c/02_atomic_counter_and_cas.c` |
| [c/03_memory_order_litmus.c](c/03_memory_order_litmus.c) | Message-passing litmus (relaxed / release-acquire / fences) and store-buffering litmus (relaxed / release-acquire / seq_cst) | `make run P=linux-internals/02-race-conditions-and-atomics/c/03_memory_order_litmus.c` |
| [c/04_treiber_stack_aba.c](c/04_treiber_stack_aba.c) | Lock-free Treiber stack on an index pool, ABA replayed step by step, tag fix, 4-thread stress | `make run P=linux-internals/02-race-conditions-and-atomics/c/04_treiber_stack_aba.c` |
| [c/05_false_sharing.c](c/05_false_sharing.c) | Two counters in one cache line vs padded vs local accumulation; timings printed | `make run P=linux-internals/02-race-conditions-and-atomics/c/05_false_sharing.c SAN=0 OPT=1` |
| [cpp/01_std_atomic.cpp](cpp/01_std_atomic.cpp) | `std::atomic` RMW vs load+store, `is_lock_free`/`is_always_lock_free`, generic CAS `fetch_update`, publishing via `atomic<T*>`, `atomic_flag` spinlock with `lock_guard`, stop flag | `make run P=linux-internals/02-race-conditions-and-atomics/cpp/01_std_atomic.cpp` |

All non-`_bug` programs also pass under ThreadSanitizer:
`make run P=linux-internals/02-race-conditions-and-atomics/c/03_memory_order_litmus.c SAN=tsan`
(or `make test-tsan` for the whole track). Cross-compile `c/03` and `c/05`
for the board (`aarch64-linux-gnu-gcc -O2 -pthread`) to see ARM's weaker
ordering and its cache-line cost.

---

## 9. Exercises

**Easy**
1. Fix `c/01_racy_counter_bug.c` three ways: `atomic_int`, a
   `pthread_mutex_t`, and per-thread counters summed after join. Time all
   three with `SAN=0 OPT=1`.
2. In `c/02`, change the `atomic_max` CAS to `_strong` and explain why the
   result is the same.
3. Print `ATOMIC_INT_LOCK_FREE`, `ATOMIC_LLONG_LOCK_FREE`,
   `ATOMIC_POINTER_LOCK_FREE` on x86 and on the board.

**Medium**
4. Implement Peterson's lock for two threads with `seq_cst` atomics; use it
   to protect a plain counter and pass TSan. Then weaken the flag stores to
   `release` and the loads to `acquire` and catch it failing
   (use the SB harness from `c/03`).
5. Write `atomic_fetch_mul` and a "decrement unless zero" (`refcount`-style)
   with CAS loops; test them from 4 threads.
6. Cross-compile `c/03_memory_order_litmus.c` for aarch64, run it on the
   BeagleBone AI-64 a few times, and record the relaxed MP anomaly count.
   Then `objdump -d` it and find the `ldar`/`stlr`/`dmb` instructions.

**Hard**
7. Turn `c/04` into a lock-free **free-list allocator** for fixed-size
   blocks used by two producer threads and one consumer. Prove (TSan +
   counting) no block is ever handed out twice.
8. Replace the tag in `c/04` with a simple **hazard pointer** scheme using
   malloc'd nodes, and show with ASan that the untagged/unprotected version
   reads freed memory.
9. Measure contended `fetch_add` throughput with 1, 2, 4, 8 threads on one
   counter vs per-thread padded counters (sharded counter) and plot it.
   Explain why the single counter gets *slower* with more threads.

---

## 10. Remember forever

```
data race = concurrent access, >=1 write, >=1 non-atomic  -> UB, TSan finds it
race condition = timing-dependent logic (check-then-act)   -> TSan does NOT find it
counter++ = LOAD, ADD, STORE.  volatile != atomic != ordered
atomic RMW: fetch_add / exchange / CAS.  CAS loop: old=load; do new=f(old) while(!cas_weak(&x,&old,new))
relaxed = atomic only.  release store ⟶ acquire load = happens-before (publish data)
store->load (Dekker/SB) needs seq_cst; release/acquire is not enough
x86: acq/rel free, seq_cst store = xchg/mfence.  ARM: ldar/stlr, ldxr/stxr loops, dmb ish
ABA: CAS compares values -> add a tag, or never recycle while in use (hazard ptr/RCU)
false sharing: hot per-thread data on its own 64-byte line
memory_order must be a compile-time constant (else GCC uses seq_cst)
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Define *data race* and *race condition*. Give an example of each without
   the other.
2. Draw the interleaving that loses an increment in `counter++`.
3. Name the three properties atomics + memory orders give you, and which
   one `memory_order_relaxed` still gives.
4. Why is `volatile int ready` not enough to publish data to another
   thread? Give two separate reasons.
5. What does `atomic_compare_exchange_weak` do with `expected` when it
   fails? Why can the weak form fail spuriously on ARM?
6. In the message-passing litmus test, which accesses need which orders,
   and what can go wrong on a Cortex-A72 with relaxed?
7. Why can `r1 == 0 && r2 == 0` happen in the store-buffering test on x86,
   and why doesn't release/acquire prevent it?
8. What do `ldar`, `stlr`, `ldxr`/`stxr` and `dmb ish` do? Which C11
   operations map to them?
9. Explain ABA on a Treiber stack and two ways to fix it.
10. Two threads increment two different counters and it is slow. What is
    happening and how do you prove it?
11. When is `std::atomic<T>` not lock-free, and why does it matter for
    shared memory between processes?
12. Why does the SPSC ring in dsa/07 need no CAS at all?

---

**Next:** [03 — Mutexes and deadlock](../03-mutexes-and-deadlock/README.md)
— when the critical section is bigger than one word, take a lock.
Previous: [01 — Processes and threads](../01-processes-and-threads/README.md).
