# 07 — Read-write locks, seqlocks and RCU

> When data is read a thousand times for every write, a mutex wastes most
> of its time making readers wait for *other readers*. This lesson goes
> step by step from "readers share, writers exclude" (rwlock), to "readers
> don't lock at all, they retry" (seqlock), to "readers don't lock and
> never retry, writers publish a new copy" (RCU). Each step makes reading
> cheaper and writing more expensive.

---

## 1. Why this exists

Typical read-mostly data on an embedded Linux box:

| Data | Reads | Writes |
|---|---|---|
| Routing table / ARP cache | every packet | a route changes |
| Camera / pipeline configuration | every frame | an operator edits a setting |
| System time (`clock_gettime`) | millions per second | every timer tick |
| List of loaded modules, mounted filesystems | every lookup | rarely |

With a mutex, two readers on the TDA4VM's two A72 cores take turns even
though neither changes anything. Worse, every lock/unlock is an atomic
write to a shared cache line, so even "uncontended" readers on different
cores slow each other down by bouncing that line. The three tools in this
lesson remove the reader-reader serialisation, and then the reader writes
as well.

---

## 2. Mental model

- **rwlock = a museum room.** Any number of visitors (readers) can be in
  the room together. To rearrange the exhibits, the curator (writer) waits
  until the room is empty and locks the door.
- **seqlock = a scoreboard with a "being updated" light.** You glance at
  the light (sequence number), copy the score, glance again. If the light
  was on, or the number changed, you copy again. The scorer never waits for
  you.
- **RCU = a newspaper.** Readers keep reading *their* copy. The editor
  prints a new edition and puts it on the stand (publishes a pointer).
  Old copies are recycled only after everyone who picked one up before the
  switch has finished reading (the *grace period*).

```
  mutex      R1 [==]  R2 [==]  R3 [==]  W [==]          everyone in turn
  rwlock     R1 [=====]                 W [==]           readers overlap,
             R2  [=====]                                 writer alone
             R3   [=====]
  seqlock    R  [copy][copy-retry][copy]                 readers never block
             W        [==write==]                        the writer
  RCU        R1 [==== old version ====]                  readers never block,
             W       publish new --- grace period --- free(old)
             R2              [==== new version ====]     never retry
```

---

## 3. How it works

### 3.1 pthread_rwlock_t  (`c/01_rwlock_basics.c`)

```c
pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
pthread_rwlock_rdlock(&rw);      /* shared: many at once            */
pthread_rwlock_wrlock(&rw);      /* exclusive                       */
pthread_rwlock_tryrdlock(&rw);   /* 0 or EBUSY                      */
pthread_rwlock_trywrlock(&rw);
pthread_rwlock_timedrdlock(&rw, &abs_realtime_deadline);   /* ETIMEDOUT */
pthread_rwlock_clockwrlock(&rw, CLOCK_MONOTONIC, &abs);     /* glibc 2.30+ */
pthread_rwlock_unlock(&rw);      /* same call for both kinds        */
```

The program *proves* readers overlap: each reader takes the read lock and
then waits, still inside, until all four readers are inside. With a mutex
that would time out; with an rwlock the maximum is 4. Then a second thread
probes the lock while main holds it for reading (other readers: yes,
writers: `EBUSY` / `ETIMEDOUT`) and for writing (nobody gets in). glibc
detects a second `wrlock` by the owning thread (`EDEADLK`).

Cost: an rwlock is *more* expensive than a mutex per operation (it tracks a
reader count and phases). It only pays off when critical sections are long
enough for readers to really overlap. For tiny critical sections a mutex
is often faster, and seqlock/RCU are faster still.

### 3.2 Writer starvation  (`c/02_rwlock_starvation_and_upgrade.c`)

glibc's default (`PTHREAD_RWLOCK_PREFER_READER_NP`) lets a new reader in
even while a writer is waiting. If readers keep overlapping, the writer
never runs:

```
   R1  [=========read=========]
   W        wrlock ... waiting ...................... (starved)
   R2               [=========read=========]      <- admitted past W
   R3                          [=========read=====]  <- and again
```

The program shows it deterministically: R1 holds, W queues, R2 is admitted,
R1 leaves, and W is *still* waiting because R2 holds the lock. The fix:

```c
pthread_rwlockattr_t a;
pthread_rwlockattr_init(&a);
pthread_rwlockattr_setkind_np(&a, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
pthread_rwlock_init(&rw, &a);
```

Now, once a writer is queued, new readers block (`tryrdlock` returns
`EBUSY`) and the writer goes next. "NONRECURSIVE" is a promise: a thread
that already holds a read lock must not take it again while a writer may be
waiting. It would block behind the writer, which waits for that very
thread: deadlock. (`PTHREAD_RWLOCK_PREFER_WRITER_NP` exists but glibc
treats it like PREFER_READER for exactly that reason.)

### 3.3 The upgrade deadlock

```
   T: rdlock()  ->  "I need to modify"  ->  wrlock()
                                              waits for all readers to leave,
                                              including T itself: forever
```

No POSIX rwlock and no `std::shared_mutex` can be upgraded in place. With
two threads doing it at the same time it is a deadlock in *any* design
(each waits for the other to drop its read lock). The fix is always the
same: **drop the read lock, take the write lock, re-check the condition**,
because anything may have changed in between. `cpp/01`'s
`get_or_compute()` is that pattern. (Boost has `upgrade_lock`: only one
thread may hold upgrade ownership at a time, which removes the two-thread
deadlock.)

### 3.4 Seqlock  (`c/03_seqlock.c`, `cpp/03_seqlock.cpp`)

```
   writer (serialised by its own mutex)      reader (never writes anything)
   s = seq                                   do {
   seq = s + 1          (odd: writing)           s1 = seq          (acquire)
   data = ...           (release stores)         if (s1 odd) retry
   seq = s + 2          (even, release)          copy data         (acquire loads)
                                                 s2 = seq
                                             } while (s1 != s2)
```

```
   seq:   0      1        2      3        4
          |  W:  [writing]  |  W: [writing]  |
   R: s1=2 ... copy ... s2=2  -> consistent
   R: s1=2 ... copy ... s2=4  -> a write happened in between: retry
   R: s1=3                    -> writer active: retry
```

Properties: writers never wait for readers (a reader cannot starve a
writer, the opposite of 3.2); readers never write shared memory (no cache
line bouncing between readers); but readers may retry forever under
constant writes, and the data **must not contain pointers** the reader
follows: the reader may copy a half-written pointer and dereference
garbage before it detects the retry.

**The C11 data-race subtlety.** The reader copies the data *while* the
writer may be writing it. In C11/C++11 that is a data race, which is
undefined behaviour, even if the torn copy is thrown away. So in user
space every payload field is an atomic, here with acquire loads and
release stores, which also gives the needed ordering without stand-alone
fences (ThreadSanitizer cannot model `atomic_thread_fence`). The kernel
uses plain memory plus `READ_ONCE()` and `smp_rmb()`/`smp_wmb()`, which
is valid under the kernel's own memory model (LKMM), not under C11. The
C++ version stores any trivially copyable `T` as an array of
`std::atomic<uint64_t>` words and `memcpy`s in and out.

### 3.5 RCU: Read-Copy-Update  (`c/04_rcu_userspace.c`, `cpp/02_rcu_shared_ptr.cpp`)

```
   readers:  p = load_acquire(&g_table); use *p          (no lock, no retry)

   writer:   new = malloc; *new = *old; modify(new)      READ, COPY, UPDATE
             store_release(&g_table, new)                PUBLISH
             synchronize_rcu()                           GRACE PERIOD
             free(old)                                   RECLAIM

   time ---------------------------------------------------------------->
   R1  [read old.......]
   R2        [read old..............]
   W            publish new | grace period: wait for R1, R2 | free(old)
   R3                 [read new....]      <- started after publish: never
                                             sees old, need not be waited for
```

Publishing is easy: a release store of the pointer (readers load it with
acquire, so they see a fully initialised object). The hard part is
**reclamation**: when is `old` safe to free? Answer: after every reader
that *could* have loaded the old pointer has finished. That interval is the
grace period. Readers that start after the publish see the new pointer,
so the writer only has to wait for readers that were already inside.

`c/04` implements this with one counter per reader, odd while it is inside
a read-side section:

```
   rcu_read_lock(me):    ctr[me]++          (odd)
   rcu_read_unlock(me):  ctr[me]++          (even)
   synchronize_rcu():    for each reader i:
                             snap = ctr[i]
                             if snap is odd: wait until ctr[i] != snap
```

The reader's increment and its pointer load, and the writer's pointer
store and its counter loads, form the classic store-then-load (Dekker)
pattern, so all four use `seq_cst`. Then for every reader, either the
writer sees it inside and waits, or the reader's load comes later in the
total order and sees the new pointer. `c/05_rcu_free_too_early_bug.c`
shows what happens without the grace period: ASan reports a
heap-use-after-free on every run.

`cpp/02` solves reclamation with reference counting instead:
`std::shared_ptr<const T>` loaded with `std::atomic_load`. The old version
is freed when the last reader drops its `shared_ptr`. Simpler, but every
read does an atomic increment and decrement on a shared counter, so it does
not scale like real RCU. Writers use `std::atomic_compare_exchange_strong`
in a loop, so several writers need no extra mutex.

**Real RCU.** In the kernel, `rcu_read_lock()` is just
`preempt_disable()` (or nothing at all on a non-preemptible kernel): the
read side costs *zero* instructions that touch shared memory. The writer's
`synchronize_rcu()` waits until every CPU has passed a quiescent state
(a context switch, idle, or user mode), which proves no reader from before
is still running. `call_rcu(&obj->rcu_head, free_fn)` defers the free
without blocking the writer; `kfree_rcu()` is the shorthand. The API:

| Kernel | Meaning | `c/04` equivalent |
|---|---|---|
| `rcu_read_lock()` / `rcu_read_unlock()` | read-side section | `ctr++` / `ctr++` |
| `rcu_dereference(p)` | load pointer (dependency ordering) | `atomic_load(seq_cst)` |
| `rcu_assign_pointer(p, v)` | publish (release) | `atomic_store` |
| `synchronize_rcu()` | wait for a grace period | `synchronize_rcu()` |
| `call_rcu()` / `kfree_rcu()` | free after a grace period, asynchronously | (exercise 8) |
| `list_for_each_entry_rcu()`, `hlist_*_rcu` | RCU-safe list traversal | |

**liburcu** (userspace-rcu.org) brings this to user space in several
flavours: QSBR (readers call `rcu_quiescent_state()` periodically; read
side is free), "memb" (uses the `membarrier(2)` syscall so readers need
only compiler barriers), and signal-based. It is used by LTTng, Knot DNS
and others, and provides RCU hash tables and queues.

### 3.6 Comparison

| | mutex | rwlock | seqlock | RCU |
|---|---|---|---|---|
| Reader cost | lock + unlock (atomic RMW ×2, shared line) | ≥ mutex (reader count RMW) | 2 loads of seq + copy; may retry | ~0 (kernel); 2 RMW on own line (`c/04`); refcount (`shared_ptr`) |
| Writer cost | lock + unlock | wait for all readers to drain | lock + 2 stores, never waits for readers | copy + publish + grace period (ms in the kernel) or `call_rcu` |
| Readers block writers? | yes | yes (and can starve them) | **no** | **no** |
| Writers block readers? | yes | yes | readers retry (spin) | **no** (they see old or new) |
| Readers see | latest | latest | latest consistent | old **or** new version for a while |
| Data shape | anything | anything | small, plain values, **no pointers** to follow | pointer to a heap object (lists, trees, tables) |
| Typical use | general default | long read sections, few writes | time, statistics, counters (`jiffies_64`, timekeeping, `u64_stats`) | routing tables, module list, dentry cache, config snapshots |

---

## 4. Complexity table

| Operation | Time | Notes |
|---|---|---|
| `pthread_rwlock_rdlock` uncontended | O(1), 1 atomic RMW | all readers hit one cache line: does not scale with cores |
| `pthread_rwlock_wrlock` | O(1) + wait for readers to drain | starvation possible with reader preference |
| Seqlock read | O(size of data) per attempt, unbounded retries | expected O(1) retries when writes are rare |
| Seqlock write | O(size of data) | never waits for readers; writers serialise on their lock |
| RCU read (`c/04`) | O(1), 2 RMW on the reader's *own* line | kernel: O(1) with no RMW at all |
| RCU update | O(size of object) copy + O(readers) grace period | readers never delayed |
| `shared_ptr` RCU read | O(1), 2 atomic RMW on the *shared* control block | contention on the refcount line |
| Memory | rwlock: 56 bytes; seqlock: 1 counter; RCU: up to 2+ versions alive during a grace period | RCU trades memory for read speed |

---

## 5. C vs C++

| Concept | C (POSIX / C11) | C++17 |
|---|---|---|
| Reader-writer lock | `pthread_rwlock_t` | `std::shared_mutex` (libstdc++: wraps `pthread_rwlock_t`); `std::shared_timed_mutex` adds `try_lock_for` |
| Read guard | `rdlock` ... `unlock` | `std::shared_lock<std::shared_mutex>` |
| Write guard | `wrlock` ... `unlock` | `std::unique_lock` / `std::lock_guard` |
| Writer preference | `pthread_rwlockattr_setkind_np` | not configurable (implementation-defined) |
| Seqlock | hand-written with `<stdatomic.h>` | hand-written template, payload in `std::atomic` words |
| RCU publish / read | `_Atomic(T*)` + release / acquire | same with `std::atomic<T*>`; or `std::atomic_load/store(&shared_ptr)` (C++11–17), `std::atomic<std::shared_ptr<T>>` (C++20) |
| Grace period | hand-written (`c/04`) or liburcu | refcount (`shared_ptr`), hazard pointers / RCU in C++26 (`<rcu>`, `<hazard_pointer>`) |

`cpp/01_shared_mutex_cache.cpp` also demonstrates a pitfall that is
independent of the lock type: two separate `get()` calls are two critical
sections, so `a + b` read that way is inconsistent thousands of times per
run, while `get_pair()` under one shared lock is always consistent.

---

## 6. Where it is used in the real world

- **Linux kernel.**
  - `rwlock_t` (spinning) and `struct rw_semaphore` (sleeping): `mmap_lock`
    (the per-process address-space lock, taken for reading on every page
    fault) is an rw_semaphore.
  - `seqlock_t` / `seqcount_t`: `jiffies_64` on 32-bit CPUs, the
    timekeeper (`kernel/time/timekeeping.c`), `u64_stats_sync` for 64-bit
    network counters on 32-bit machines, `rename_lock` in the dcache. The
    **vDSO** runs `clock_gettime()` in user space by reading the kernel's
    time data under a seqcount: no syscall at all.
  - RCU everywhere: the routing table (FIB) lookup on every packet, the
    dentry cache (path lookup is "RCU-walk"), the module list, the
    PID hash, netfilter rules, device lists. `rcu_read_lock()` /
    `synchronize_rcu()` / `call_rcu()` / `kfree_rcu()`. Driver-side usage
    is in
    [linux-device-drivers/03-kernel-synchronization](../../linux-device-drivers/03-kernel-synchronization/README.md).
- **glibc / user space.** `pthread_rwlock_t`; liburcu (LTTng, Knot DNS,
  GlusterFS); `std::shared_mutex`; databases use reader-writer latches on
  B-tree pages.
- **GStreamer / GLib.** `GRWLock` (on `pthread_rwlock_t`); the plugin
  registry and element factories are read-mostly; GStreamer elements often
  copy a property struct under `GST_OBJECT_LOCK` and use the copy outside:
  a hand-made "read-copy" snapshot. Threads and queues:
  [gstreamer/08-queues-threads-tee](../../gstreamer/08-queues-threads-tee/README.md).
- **Embedded/RTOS.** FreeRTOS has no rwlock; people use a mutex, or a
  double buffer with an index flip (a poor man's RCU: write the inactive
  buffer, then switch the pointer, valid only if readers finish within a
  known time). Sensor fusion / timestamp data shared between an ISR and a
  task is the seqlock pattern (on a single core the "writer" is the ISR
  and the reader simply re-reads until two reads agree).

---

## 7. Common mistakes and pitfalls

1. **Using an rwlock for tiny critical sections.** It costs more than a
   mutex per operation; it wins only when readers really overlap.
2. **Writer starvation** with the default reader-preferring
   `pthread_rwlock_t`. Use `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`
   (and never take a read lock recursively).
3. **Upgrading a read lock** (`rdlock` then `wrlock`): self-deadlock. Drop,
   re-lock for writing, **re-check**.
4. **Returning references out of a read-locked section** (`const std::string&`
   from a map): dangling once the lock is released and a writer modifies the
   map. Return copies or snapshots.
5. **Several reads in several critical sections** and expecting a
   consistent combination (`cpp/01` counts how often that breaks).
6. **Seqlock with pointers**: the reader may follow a torn pointer before
   it notices it must retry. Copy plain values only.
7. **Seqlock payload as plain variables in C/C++**: data race = UB. Use
   relaxed/acquire atomics (or the kernel's READ_ONCE under LKMM).
8. **Seqlock with several writers and no writer lock**: two writers make
   the counter odd/even in the wrong order and readers accept torn data.
9. **Seqlock reader retrying forever** when writes never stop (or the
   writer is preempted in the middle of an update in user space). Yield in
   the retry loop; keep writes short.
10. **RCU: freeing before the grace period** (`c/05_rcu_free_too_early_bug.c`,
    ASan: heap-use-after-free).
11. **RCU: modifying the published object in place.** Readers see a
    half-modified object. Always copy, then publish.
12. **RCU: holding a reference after `rcu_read_unlock()`** (using `p` after
    the read section). The grace period no longer protects it.
13. **TSan and the `*clock*` lock variants.** GCC 11's ThreadSanitizer
    does not intercept the glibc 2.30+ functions `pthread_rwlock_clockrdlock/
    clockwrlock`, `pthread_cond_clockwait` and `pthread_mutex_clocklock`
    (libstdc++ uses the last two for `wait_for`/`try_lock_for` with
    `steady_clock`). A lock taken through them is invisible to TSan, which
    can then print false "data race" / "unlock of an unlocked mutex"
    reports. The rwlock programs here use `clockwrlock` and stay clean
    under TSan, but if you see such a report around a `clock*` call,
    switch to the `timed*` variant (CLOCK_REALTIME) under TSan before
    hunting for a real bug.
14. **Forgetting acquire/release on the published pointer**: on the A72 a
    reader can see the new pointer but stale contents of the object. Run
    `make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/04_rcu_userspace.c SAN=tsan`
    after any change.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_rwlock_basics.c](c/01_rwlock_basics.c) | rdlock/wrlock, try/timed/clock variants, proof that 4 readers are inside at once, invariant stress test | `make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/01_rwlock_basics.c` |
| [c/02_rwlock_starvation_and_upgrade.c](c/02_rwlock_starvation_and_upgrade.c) | Reader-preferring lock admits readers past a waiting writer; PREFER_WRITER_NONRECURSIVE refuses them; upgrade deadlock shown with a timed lock | `make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/02_rwlock_starvation_and_upgrade.c` |
| [c/03_seqlock.c](c/03_seqlock.c) | Seqlock with C11 atomics (odd = writing, retry), 1 and 2 writers, zero torn reads | `make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/03_seqlock.c` |
| [c/04_rcu_userspace.c](c/04_rcu_userspace.c) | User-space RCU: per-reader counters, copy-modify-publish, `synchronize_rcu()`, then `free(old)` | `make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/04_rcu_userspace.c` |
| [c/05_rcu_free_too_early_bug.c](c/05_rcu_free_too_early_bug.c) | **Bug:** free without a grace period; ASan (and TSan) report heap-use-after-free | `make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/05_rcu_free_too_early_bug.c` |
| [cpp/01_shared_mutex_cache.cpp](cpp/01_shared_mutex_cache.cpp) | `std::shared_mutex` config cache: `shared_lock`, `unique_lock`, get-or-compute with re-check, consistent multi-key reads | `make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/cpp/01_shared_mutex_cache.cpp` |
| [cpp/02_rcu_shared_ptr.cpp](cpp/02_rcu_shared_ptr.cpp) | RCU with `shared_ptr<const T>` + `std::atomic_load` / CAS: snapshots, multi-writer CAS loop, automatic reclamation | `make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/cpp/02_rcu_shared_ptr.cpp` |
| [cpp/03_seqlock.cpp](cpp/03_seqlock.cpp) | Generic `Seqlock<T>` for trivially copyable T (camera frame timestamps) | `make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/cpp/03_seqlock.cpp` |

All programs except the `_bug` are clean under ThreadSanitizer, e.g.
`make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/03_seqlock.c SAN=tsan`
(or `make test-tsan` for the whole track).

---

## 9. Exercises

**Easy**
1. In `c/01`, replace the rwlock by a `pthread_mutex_t` and watch the
   concurrent-reader test time out (it is bounded to 5 s): that is the
   difference in one experiment.
2. In `c/03`, remove the writer mutex and run with 2 writers. Do torn reads
   appear? Explain with a timeline why they can.
3. Change `cpp/01`'s `get()` to return `const std::string&`. Why does it
   compile, and why is it a bug? Can TSan or ASan show it?

**Medium**
4. Measure reads per second for mutex vs rwlock vs seqlock vs RCU with 1, 2,
   4 reader threads (`make run ... SAN=0 OPT=1`), with a short and a long
   read-side section. Plot it. Repeat on the BeagleBone.
5. Make the seqlock in `c/03` use relaxed data accesses plus
   `atomic_thread_fence(memory_order_acquire/release)` (the kernel's
   shape). Is it still correct? Why can TSan not verify it?
6. Add `rcu_register_thread()` to `c/04` so readers can come and go
   dynamically (a registry of slots protected by a mutex that
   `synchronize_rcu` also takes).

**Hard**
7. Build an RCU-protected singly linked list in `c/04` style: lock-free
   lookup, insert/delete under a writer mutex, deleted nodes freed after
   `synchronize_rcu()`.
8. Implement `call_rcu()`: a reclaimer thread that batches retired objects
   and frees them all after ONE grace period (amortises the cost, like the
   kernel). Verify with ASan.
9. Implement QSBR: readers call `rcu_quiescent_state()` between operations
   instead of lock/unlock. Show that the read side has no atomic RMW at all.

---

## 10. Remember forever

```
rwlock : many readers OR one writer. glibc default prefers readers -> writer starvation;
         PREFER_WRITER_NONRECURSIVE_NP fixes it. NO upgrade: unlock, wrlock, RE-CHECK.
seqlock: W: seq++ (odd); write; seq++ (even).  R: s1=seq; if odd retry; copy; if seq!=s1 retry.
         writers never wait; no pointers in data; C11: payload must be atomics.
RCU    : R: p = load_acquire(ptr); use *p.   W: copy, modify, store_release(ptr, new),
         synchronize_rcu() (wait for readers that were inside), free(old).
         kernel: rcu_read_lock = preempt_disable; call_rcu/kfree_rcu defer the free.
cost   : reads  mutex > rwlock > seqlock > RCU ;  writes  the other way round.
use    : time/stats -> seqlock ; routing tables/lists -> RCU ; long reads -> rwlock.
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. When is an rwlock *slower* than a mutex? Why do all readers still
   contend on something?
2. Draw the timeline of writer starvation. Which attribute fixes it, and
   what does "NONRECURSIVE" forbid?
3. Why can a read lock not be upgraded to a write lock? What is the
   correct pattern?
4. Walk through a seqlock read that overlaps a write. Why is an odd
   sequence number checked separately?
5. Why must the payload of a user-space seqlock be atomic in C11, while
   the kernel uses plain memory?
6. Why must seqlock-protected data not contain pointers that the reader
   dereferences?
7. Explain RCU's publish step. What goes wrong on ARM without
   release/acquire?
8. What is a grace period? Which readers must the writer wait for, and
   which may it ignore?
9. In `c/04`, why do the reader's counter increment and pointer load
   need `seq_cst` and not just release/acquire?
10. How does the kernel make `rcu_read_lock()` free? What is a quiescent
    state?
11. Compare reclamation by grace period and by reference counting
    (`shared_ptr`). Which scales better with many reader cores, and why?
12. For each, pick mutex/rwlock/seqlock/RCU and justify: system time,
    a routing table, a 3-field sensor sample written by one thread, a
    config map read by 8 threads and edited by an operator.
