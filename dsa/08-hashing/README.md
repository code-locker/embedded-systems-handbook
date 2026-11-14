# 08 — Hashing

> One sentence to keep: **a hash table turns "search" into "compute where it
> must be, then look there."**

---

## 1. Why this exists

An array gives O(1) access, but only by a small integer index. Real lookups
use names: `"eth0"` to its MTU, a PID to its `task_struct`, an IP address to
its socket, a symbol name to its address. A sorted array or a tree finds those
in O(log n). A **hash table** finds them in **O(1) on average**. It *computes*
an array index from the key (the hash), so it never searches.

You pay three things for that speed:

- **No order.** Keys come out in a scrambled order, and you cannot ask for
  "all keys between A and B".
- **Extra memory.** Empty slots and buckets are kept on purpose.
- **A bad worst case.** If many keys land in the same spot, an operation
  becomes O(n).

The whole art of hashing is keeping that worst case away.

---

## 2. Mental model

**The coat-check desk.** You hand over a coat. The attendant does not search
the racks. They look at your ticket number and walk straight to hook
`number % hooks`. Two tickets can map to the same hook (a *collision*). In that
case the attendant either hangs several coats on one hook (**chaining**) or
uses the next free hook (**open addressing**).

```
 key "eth0" ──► hash() ──► 0x9A3F_17C2 ──► & (8-1) ──► index 2
                                                          │
   buckets / slots:   0     1     2     3     4     5     6     7
                    ┌────┬─────┬─────┬────┬─────┬────┬─────┬────┐
                    │    │     │eth0 │    │wlan0│    │ lo  │    │
                    └────┴─────┴─────┴────┴─────┴────┴─────┴────┘
                                  ▲
                    lookup("eth0") computes 2 again and looks ONLY there
```

There are three parts, and you must be able to name all three:

1. **Hash function**: key → big integer. It must be *deterministic*, fast,
   and spread keys evenly.
2. **Index mapping**: big integer → slot (`% m` or `& (m-1)`).
3. **Collision strategy**: what to do when two keys want the same slot.

A fourth part drives performance: the **load factor α = n / m**, the number
of items per slot. Keep α bounded (by resizing), and every operation stays
O(1) on average.

---

## 3. How it works

### 3.1 Hash functions

A good hash function has **avalanche**: change one input bit, and about half
of the output bits flip. Then similar keys (`sensor_1`, `sensor_2`) land far
apart.

| Function | Core step | Notes |
|---|---|---|
| **djb2** | `h = h*33 + c`, start 5381 | Tiny and fast. Its low bits mix weakly, so similar keys cluster |
| **FNV-1a** | `h ^= c; h *= 16777619`, start 2166136261 | XOR then multiply. Good spread, a great default for strings |
| **Multiplicative (Fibonacci)** | `(x * 0x61C88647) >> (32 - bits)` | The kernel's `hash_32()`. Uses the **top** bits |
| **Murmur finaliser** | xor-shift / multiply rounds | Turns a weak integer hash into a strong one |
| **SipHash** | keyed (secret seed) | Resists *hash flooding* attacks. Used by Python, Rust, and kernel net tables |
| sum of bytes | `h += c` | **Bad.** Anagrams collide and the value range is tiny |

Measured by `c/01_hash_functions.c`: 4096 keys of the form `sensor_N` into
about 1024 buckets.

```
hash   index    used buckets   longest chain
sum    % 1021        85            231     ← useless
djb2   % 1021       677             17
fnv1a  & 1023       948             12     ← ideal is ~1000 / ~4-8
```

### 3.2 Hash → index: prime modulo vs power-of-two mask

```
 h % m  (m prime, e.g. 1021)      every bit of h influences the result
                                  cost: one division (tens of cycles)

 h & (m-1)  (m = 2^k, e.g. 1024)  ONE AND instruction, but keeps only the
                                  low k bits:
        h   = 1011 0110 1100 0000
        m-1 = 0000 0011 1111 1111
        idx = 0000 0010 1100 0000      high bits thrown away
```

**The trap:** if the keys are multiples of 64 (aligned addresses, buffer
indices) and the hash is the identity (libstdc++'s `std::hash<int>` *is* the
identity), then the low 6 bits are always 0. Only **16 of 1024** buckets are
ever used. The program shows this. A prime modulus rescues it, and so does
mixing first.

Rule: **power-of-two mask only with a hash that mixes well into its low bits**
(FNV, Murmur). Otherwise take the *top* bits of a multiplicative hash, as the
kernel does. libstdc++'s `unordered_map` uses prime bucket counts for exactly
this reason. Google's SwissTable, Rust, and most C hand-rolled tables use
powers of two plus a strong hash.

### 3.3 Separate chaining (`c/02_chaining_map.c`)

Each bucket is the head of a linked list.

```
 buckets ──► [0] NULL
             [1] ──► {"a",1} ──► NULL
             [2] ──► {"z",26} ──► {"b",2} ──► NULL      collision: both hash to 2
             [3] NULL
```

- **put**: hash, walk the chain. If the key exists, update it. Otherwise
  **push at the head** (O(1)).
- **get**: hash, walk the chain, compare the cached hash first and `strcmp`
  only on a match.
- **delete**: the *pointer-to-pointer* trick. `pp` points at whichever link
  points to the node, so deleting the head and deleting a middle node are the
  same code: `*pp = node->next; free(node);`.
- Chaining tolerates α ≥ 1 well. `std::unordered_map` uses `max_load_factor = 1.0`.

### 3.4 Open addressing (`c/03_open_addressing.c`)

There are no lists. Entries live **in the slot array itself**. On a collision
you **probe** a sequence of slots.

```
 probe i = 0,1,2,...       home slot h = hash(key) & (m-1)

 linear     h, h+1, h+2, h+3 ...        cache-friendly; full runs merge into
                                        long "primary clusters"
 quadratic  h, h+1, h+3, h+6, h+10 ...  (h + i(i+1)/2) breaks primary clusters;
                                        triangular steps visit every slot of a 2^k table
 double     h, h+s, h+2s, ...           s = hash2(key), odd → visits all slots;
                                        colliding keys take DIFFERENT paths
```

**Tombstones.** You cannot simply empty a deleted slot:

```
  slot:   1     2     3     4     5          A, B, C all have home slot 2
        [   ] [ A ] [ B ] [ C ] [   ]
  delete B by emptying →  [ A ] [   ] [ C ]   get(C) stops at the hole: "absent"  ✗
  delete B by tombstone → [ A ] [ † ] [ C ]   get(C) walks past †: found        ✓
```

- A lookup **stops at EMPTY** and **continues past TOMBSTONE**.
- An insert may **reuse** the first tombstone it passed.
- Tombstones still lengthen probes, so count them in the load. A rehash
  throws them all away.
- Keep α ≤ 0.5–0.7. The expected number of probes for a miss under linear
  probing is about ½(1 + 1/(1-α)²), which explodes as α approaches 1.

### 3.5 Load factor and rehashing

```
 α = n / m      if α > max  →  allocate 2m, re-insert every entry (O(n))
```

Each element is moved again only after the table has doubled. That is the
same amortised argument as `vector` growth, so the **amortised cost of an
insert stays O(1)**. Notes:

- Chaining moves *nodes* (only pointers change). Open addressing re-inserts
  values.
- Rehash **invalidates iterators** (and, for open addressing, element
  addresses).
- `reserve(n)` up front avoids all rehashes when you know n.
- Real-time / embedded: a rehash is a latency spike. Pre-size, or rehash
  incrementally (Redis moves a few buckets per operation).

### 3.6 Robin Hood hashing (advanced, `c/04_robin_hood.c`)

This is linear probing with one rule: **take from the rich, give to the
poor.** Each entry stores its *distance* from its home slot. While inserting,
if the resident entry is closer to home ("richer") than the one you carry,
swap them and carry the resident onward.

```
 slot:     2      3      4      5        insert X (home 2)
         [A:0]  [B:1]  [C:0]  [   ]
 X d=0 vs A0, d=1 vs B1 → move on;  d=2 vs C0 → C is richer: SWAP
         [A:0]  [B:1]  [X:2]  [C:1]     worst distance 2 instead of 3
```

What you gain, measured at 90% load:

```
               max dist   avg probes(hit)  avg probes(miss)
 linear           297          5.17            43.37
 robin hood        24          5.17             5.81
```

- The average stays the same, but the **variance collapses**.
- **Early exit on a miss**: if you reach an entry with a smaller distance
  than yours, your key cannot be further along.
- **Backward-shift delete**: slide the following entries back one slot. No
  tombstones are needed.

### 3.7 Bloom filter (`c/06_bloom_filter.c`)

A Bloom filter is an *approximate set* in very little memory.

- **add** sets k bits.
- **query** checks those k bits. If any bit is 0, the key is **definitely
  absent**. If all are 1, it is **probably present**.

```
 m=16 bits, k=3
 add cat → bits 1,6,13      add dog → bits 3,6,10
 0 1 0 1 0 0 1 0 0 0 1 0 0 1 0 0
 query eel → 1,3,10  all set → "maybe" (FALSE POSITIVE)
 query fox → 2,...   bit 2 = 0 → "no"  (always correct)
```

Sizing for n items and false-positive rate p is `m = -n·ln p / (ln 2)²` and
`k = (m/n)·ln 2`. For **1%, that is ~9.6 bits per item and k = 7**, no matter
how big the keys are. You cannot delete from a plain Bloom filter, because a
bit may be shared by several keys. A counting Bloom filter can delete.

### 3.8 The "seen-before" pattern (`c/07_applications.c`, `cpp/04_applications.cpp`)

Almost every hashing interview problem is "remember what I have seen, so
each later question costs O(1)":

| Problem | What the hash stores | O(n²) → |
|---|---|---|
| Two-sum | value → index; ask for `target - x` | O(n) |
| Frequency / most common | value → count | O(n) |
| Dedup keeping order | set of seen values | O(n) |
| Group anagrams | sorted word → list | O(n·L log L) |
| Subarray sum = k | prefix sum → count; ask for `sum - k` | O(n) |
| Longest consecutive run | set; start only where `x-1` is absent | O(n) |
| Longest substring without repeats | char → last index | O(n) |
| LRU cache | key → list iterator | O(1) per op |

When the key space is tiny (bytes, letters), the "hash table" is a plain
array indexed by the key, `int count[256]`. That is a *perfect hash*.

---

## 4. Complexity table

| Operation | Chaining avg | Open addr. avg | Worst (all collide) | Why |
|---|---|---|---|---|
| insert | O(1)* | O(1)* | O(n) | Hash + a short chain or probe. *amortised, including rehash |
| lookup hit | O(1 + α) | O(1/(1-α)) | O(n) | Expected chain length is α |
| lookup miss | O(1 + α) | O(1/(1-α)²) linear | O(n) | A miss walks the whole chain or cluster |
| delete | O(1) | O(1) (tombstone) | O(n) | Find, then unlink or mark |
| rehash | O(n) | O(n) | O(n) | Every entry is re-placed |
| iterate all | O(n + m) | O(m) | | Must visit empty buckets too |
| min / max / range | O(n) | O(n) | | No order. Use a tree |
| **space** | n nodes + m ptrs | m slots (m ≈ 2n) | | Chaining pays one pointer and one malloc per node |
| Bloom add/query | O(k) | | | k bit probes, m bits total |

---

## 5. C vs C++

| | C (this lesson) | C++ |
|---|---|---|
| Hash map | Hand-rolled: `struct entry *buckets[]` (`02`) or slot array (`03`) | `std::unordered_map<K,V,Hash,Eq>` |
| Set | Map with no value, or a `used[]` flag | `std::unordered_set<K>` |
| Multi-values | Chain all equal keys | `std::unordered_multimap` |
| Generic | `void*` + function pointers, or macros | Templates (`cpp/01_hash_map_template.cpp`) |
| Memory | You `malloc` and `free` every node and key | Nodes owned by the container (or `unique_ptr` in our own) |
| Custom key | Write `hash(struct)` and `eq(struct)` | Hasher functor *or* specialise `std::hash<T>`, plus `operator==` |
| Ordered map | Write a tree (lesson 09/12) | `std::map` (red-black tree) |
| Intrusive | `hlist_node` + `container_of` (`05`) | Boost.Intrusive |

**Hashing a struct in C++.** Combine the field hashes with
`seed ^= h + 0x9e3779b9... + (seed<<6) + (seed>>2)` (boost's `hash_combine`).
**Never plain XOR**: then `h(1,2) == h(2,1)` and `h(x,x) == 0`.

**`operator[]` inserts.** `int v = m["missing"];` silently adds the key with
value 0. Use `find`, `count`, or `at` to read. Use `try_emplace` /
`insert_or_assign` to write deliberately.

**map vs unordered_map** (`cpp/03_map_vs_unordered_map.cpp`):

| need | pick |
|---|---|
| sorted iteration, `lower_bound`, nearest key, min/max | `std::map` |
| guaranteed O(log n) worst case (real-time, adversarial input) | `std::map` |
| key has `<` but no hash (e.g. `std::pair`) | `std::map` (or write a hasher) |
| only point lookups, large n | `std::unordered_map` (2–5× faster) |
| small n, read-mostly | sorted `std::vector` + `binary_search` |

**Invalidation:** in `std::unordered_map`, a rehash invalidates *iterators*
but **never pointers or references to elements**, because nodes do not move.
`erase` invalidates only the erased element. Open-addressing tables
(`absl::flat_hash_map`) move elements and give no such guarantee.

---

## 6. Where it is used in the real world

- **Linux kernel `hlist`** (`include/linux/list.h`, `include/linux/hashtable.h`):
  PID lookup, the inode and dentry caches, `net_device` by name and index,
  and the socket lookup tables. The bucket head is **one pointer** (`hlist_head`),
  half the size of `list_head`, which matters for tables with millions of
  buckets. The node is embedded in the object, and `container_of` recovers
  the object. `pprev` gives O(1) delete without knowing the bucket.
  `hash_32()` / `hash_long()` are multiplicative hashes using the golden
  ratio. Network tables use `siphash` / `jhash` with a random seed against
  flooding. → `c/05_kernel_hlist.c`
- **Caches**: the page cache (XArray, a radix tree, today), the dentry cache,
  CPU caches (the index *is* a hash of the address bits, so "set
  associative" is a tiny hash table with a fixed chain length). Also
  memcached, Redis (chained tables with incremental rehash), and LRU caches
  (hash + list). → `cpp/05_lru_cache.cpp`
- **Databases**: hash joins (build a hash table on the smaller relation and
  probe it with the other), hash indexes, and PostgreSQL's in-memory
  `HashAggregate`. LSM-tree stores (RocksDB, LevelDB, Cassandra) keep a
  **Bloom filter per SSTable**, so a lookup skips files that certainly do not
  hold the key.
- **Compilers and interpreters**: **symbol tables**, one hash map per scope;
  string interning; Python dicts (open addressing, compact layout); Lua
  tables; V8 hidden classes.
- **Networking**: connection tracking (netfilter conntrack, a hash of the
  5-tuple), routing caches, ARP / neighbour tables, NIC **RSS** (the Toeplitz
  hash of the 5-tuple picks the RX queue / CPU).
- **Bloom filters**: Chrome's old malicious-URL check, Bitcoin SPV clients,
  CDN "one-hit-wonder" filtering, and Squid cache digests.
- **Integrity / dedup**: content-addressable storage (git object IDs,
  Docker layers), rsync rolling hashes, ZFS/btrfs dedup. These use
  *cryptographic* hashes (SHA), a different family from table hashes. Never
  use FNV/djb2 for security.
- **GStreamer / GLib**: `GHashTable` (open addressing with a prime modulus
  and tombstones) holds element properties, caps caches, and registry lookups.
  `GQuark` interns strings so later comparisons are integer compares.

---

## 7. Common mistakes and pitfalls

1. **Weak hash + power-of-two mask.** Aligned or structured keys fill only a
   few buckets. Mix the bits, or use the top bits, or use a prime size.
2. **Combining field hashes with XOR.** Symmetric, and zero for equal fields.
3. **Hash and equality disagree.** If `a == b`, then `hash(a)` must equal
   `hash(b)`. Hashing a struct's raw bytes includes **padding**, which is
   garbage, so equal structs hash differently. Hash field by field.
4. **Mutating a key while it is in the table.** It now lives in the wrong
   bucket and can never be found. That is why `std::unordered_map` makes the
   key `const`.
5. **Deleting in open addressing by emptying the slot.** Later lookups stop
   early. Use tombstones or backward shift.
6. **Ignoring tombstones in the load factor.** The table fills up with †s and
   every miss scans everything.
7. **`operator[]` to *read*** inserts default values and grows the map.
8. **Holding iterators across inserts.** A rehash invalidates them. With open
   addressing it also invalidates pointers.
9. **Erasing while iterating** without using the iterator `erase()` returns.
10. **Assuming iteration order.** It is unspecified and changes after a
    rehash. Tests that pass "by luck" break on another libc++.
11. **Hash flooding.** Attacker-chosen keys that all collide turn O(1) into
    O(n). Use a keyed hash (SipHash) on anything fed by the network.
12. **Signed overflow in a hash.** Compute hashes in `uint32_t`/`uint64_t`.
    Unsigned wrap is defined; signed overflow is UB, and UBSan flags it.
13. **Rehash latency in real-time code.** Pre-size (`reserve`), or grow
    incrementally.
14. **Expecting a Bloom filter to say "yes" reliably.** Only its "no" is
    certain.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_hash_functions.c](c/01_hash_functions.c) | djb2, FNV-1a, sum (bad); `%` prime vs `&` mask; the aligned-keys trap; kernel Fibonacci hash | `make run P=dsa/08-hashing/c/01_hash_functions.c` |
| [c/02_chaining_map.c](c/02_chaining_map.c) | Separate-chaining string→int map, pointer-to-pointer delete, load factor 0.75, rehash | `make run P=dsa/08-hashing/c/02_chaining_map.c` |
| [c/03_open_addressing.c](c/03_open_addressing.c) | Linear / quadratic / double hashing, tombstones, probe-count comparison | `make run P=dsa/08-hashing/c/03_open_addressing.c` |
| [c/04_robin_hood.c](c/04_robin_hood.c) | Robin Hood insert, early-exit lookup, backward-shift delete; vs linear at 90% load | `make run P=dsa/08-hashing/c/04_robin_hood.c` |
| [c/05_kernel_hlist.c](c/05_kernel_hlist.c) | Mini `hlist_head`/`hlist_node`, `pprev`, `container_of`, `hash_32`; one object in two tables | `make run P=dsa/08-hashing/c/05_kernel_hlist.c` |
| [c/06_bloom_filter.c](c/06_bloom_filter.c) | Bloom filter with optimal m and k, double-hashing trick, measured false-positive rate | `make run P=dsa/08-hashing/c/06_bloom_filter.c` |
| [c/07_applications.c](c/07_applications.c) | Two-sum, dedup, longest consecutive run, first unique char, anagram, most frequent | `make run P=dsa/08-hashing/c/07_applications.c` |
| [cpp/01_hash_map_template.cpp](cpp/01_hash_map_template.cpp) | `HashMap<K,V,Hash,Eq>` with `unique_ptr` chains, rehash, iterator, `operator[]` | `make run P=dsa/08-hashing/cpp/01_hash_map_template.cpp` |
| [cpp/02_unordered_containers.cpp](cpp/02_unordered_containers.cpp) | `unordered_map`/`set`/`multimap` API, buckets, invalidation, custom hasher, `std::hash` specialisation | `make run P=dsa/08-hashing/cpp/02_unordered_containers.cpp` |
| [cpp/03_map_vs_unordered_map.cpp](cpp/03_map_vs_unordered_map.cpp) | Order, range queries, timing, key requirements, worst case | `make run P=dsa/08-hashing/cpp/03_map_vs_unordered_map.cpp` |
| [cpp/04_applications.cpp](cpp/04_applications.cpp) | Two-sum, group anagrams, top-k (bucket), subarray-sum = k, longest unique substring, dedup | `make run P=dsa/08-hashing/cpp/04_applications.cpp` |
| [cpp/05_lru_cache.cpp](cpp/05_lru_cache.cpp) | LRU cache = `std::list` + `unordered_map` of iterators, page-cache simulation | `make run P=dsa/08-hashing/cpp/05_lru_cache.cpp` |

---

## 9. Exercises

**Easy**
1. Add `map_foreach(m, callback)` to `c/02_chaining_map.c`. *Hint: the
   outer loop runs over buckets, the inner loop over the chain.*
2. Count the collisions djb2 produces on the words of `/usr/share/dict/words`
   (if present) for m = 1021 and m = 1024. *Hint: reuse `measure()`.*
3. Make `c/02_chaining_map.c` shrink when α < 0.125. *Hint: hysteresis.
   Grow at 0.75 and shrink at 0.125, so a single insert/delete cannot cause
   repeated resizes.*

**Medium**
4. Implement `std::hash` for `struct Ipv4Flow { uint32_t src, dst; uint16_t sport, dport; uint8_t proto; }`
   and count flows in an `unordered_map`. *Hint: hash fields, not bytes
   (padding!).*
5. Turn `c/03_open_addressing.c` into a string→int map. *Hint: store the
   cached hash in the slot and compare it before `strcmp`.*
6. Add a counting Bloom filter (4-bit counters) that supports `remove`.
   *Hint: what happens when a counter would overflow 15?*
7. Group the lines of a log by a "normalised" key (digits replaced by `#`).
   *Hint: the canonical-key trick from group anagrams.*

**Hard**
8. Implement incremental rehashing (Redis style): keep the old and new
   tables, and move one bucket per operation. *Hint: lookups must check
   both tables.*
9. Implement a **cuckoo hash** table: two tables and two hashes, where
   insert kicks out the occupant. *Hint: detect cycles with a kick limit,
   then rehash with new seeds.*
10. Add an open-addressing `FlatMap<K,V>` to `cpp/01_hash_map_template.cpp`
    and benchmark it against `std::unordered_map` with `make OPT=1 SAN=0`.
    *Hint: one contiguous array, so far fewer cache misses.*

---

## 10. Remember forever

```
HASH TABLE = hash(key) → index → handle collision.     α = n/m, keep bounded.
  index:  h % prime (robust)   |  h & (2^k-1) (fast, needs a well-mixed hash)
  chain:  bucket = linked list; push front; delete with **pp; α ≤ 1
  open:   probe h, h+1.. (linear) | h+i(i+1)/2 | h+i*h2 (h2 odd)
          delete = TOMBSTONE (lookup skips it, insert reuses it); α ≤ 0.5–0.7
  grow:   α > max → 2m, reinsert all: O(n) once, O(1) amortised
  robin hood: swap with the "richer" (smaller dist) → tiny worst case
  FNV-1a: h=2166136261; for c: h^=c; h*=16777619
  bloom: k bits per key; any 0 → NO for sure; all 1 → maybe. ~10 bits/key = 1%
  kernel: hlist_head = 1 ptr; node has **pprev → O(1) del; container_of
```

Review on the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. What three parts make up a hash table, and what number controls its speed?
2. Why is `h & (m-1)` dangerous with `std::hash<int>` on keys that are
   multiples of 64? Give two fixes.
3. Draw a chained table after inserting three keys that collide, then delete
   the middle one with the pointer-to-pointer trick.
4. Why does open addressing need tombstones? Why do tombstones count toward
   the load factor?
5. Why must the double-hashing step be odd when m is a power of two?
6. Why is an insert O(1) amortised even though a rehash is O(n)?
7. What does Robin Hood hashing improve: the average or the worst case? How
   does it make a miss end early?
8. A Bloom filter says "yes". What do you know? It says "no". What do you
   know? Why can you not delete from it?
9. Why does the kernel's `hlist_node` have `pprev` (a `**`) instead of a
   `prev` pointer?
10. When would you choose `std::map` over `std::unordered_map`? Give three
    reasons.
11. Which `std::unordered_map` operations invalidate iterators? Which
    invalidate references?
12. How does two-sum go from O(n²) to O(n)? Why check the map *before*
    inserting the current element?
