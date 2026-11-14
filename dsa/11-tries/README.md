# 11 — Tries (prefix trees), radix trees and longest-prefix match

> A trie does not store keys. It stores **paths**. The key *is* the path from
> the root to a node, spelled one character (or one bit) per edge.

---

## 1. Why this exists

Hash tables answer one question very well: *"is this exact key present?"*
They are useless for the questions that come up constantly in real systems:

- *"Which commands start with `gst-`?"* (shell tab completion, IDE autocomplete)
- *"Which route has the **longest** prefix matching 10.20.30.41?"* (every IP router, the Linux FIB)
- *"Give me all keys in sorted order"* or *"how many words start with `pre`?"*
- *"Find the cached page for file offset N"* (Linux page cache, `xarray`)

A **trie** answers all of them in time proportional to the **length of the
key** (L), independent of how many keys are stored (n). A million words or ten
— looking up `"make"` costs 4 steps.

| Question                       | Hash table        | Balanced BST / std::set | Trie            |
|--------------------------------|-------------------|-------------------------|-----------------|
| exact lookup                   | O(L) average      | O(L log n)              | **O(L)** worst  |
| all keys with prefix P         | O(n·L) full scan  | O(L log n + output)     | **O(P + output)** |
| longest stored prefix of a key | try every prefix  | awkward                 | **O(L)**, one walk |
| sorted iteration               | no                | yes                     | yes (DFS)       |

---

## 2. Mental model

**Analogy: a phone's T9 / a library card catalogue drawer system.** You open
the drawer `C`, inside it the drawer `A`, inside that `R`... Every word that
starts with `CA` lives somewhere behind the `C → A` drawers. Two words only
diverge where their spellings diverge. A small flag on a drawer says "a whole
word ends exactly here".

Words `{car, cart, care, cat, do, dog}`:

```
                    (root)
                   /      \
                 c          d
                 |          |
                 a          o*        * = "a word ends here" (is_end flag)
               /   \        |
              r*    t*      g*
             / \
            e*  t*

   path root-c-a-r      = "car"   (is_end set  -> word)
   path root-c-a        = "ca"    (no flag     -> only a prefix, NOT a word)
   path root-c-a-r-t    = "cart"
```

Three facts to burn in:

1. **Shared prefixes are stored once.** `car`, `cart`, `care`, `cat` share `c-a`.
2. **The flag is essential.** `car` is a prefix of `cart`; the node for `r`
   exists either way. Only `is_end` says whether `car` itself was inserted.
3. **Children in alphabetical order ⇒ DFS prints keys sorted.** A trie is an
   implicitly sorted set.

### Node layouts (the big design decision)

```
 (a) fixed array — c/01, c/02, cpp/01           (b) map / hash of children — cpp/02
 +---------------------------------------+       +--------------------------+
 | kid['a'] kid['b'] ... kid['z']  end   |       | std::map<char, Node*>    |
 +---------------------------------------+       | end                      |
   26 * 8 = 208 B, child lookup O(1)               +--------------------------+
   mostly NULL pointers!                            size ~ #children, lookup O(log σ)

 (c) first-child / next-sibling — c/03           (d) compressed (radix) — c/04, cpp/03
 +----+-----+-------+------+                       edge labels are STRINGS:
 | c  | end | child | next |  24 B                      root
 +----+-----+-------+------+                             |"r"
   children = sorted linked list                         o
   lookup O(σ) walk, any byte value                   "om"/ \"ub"
                                                       ...  ...
```

| Layout             | Bytes / node (64-bit) | Find child   | Alphabet       | Use when                        |
|--------------------|-----------------------|--------------|----------------|---------------------------------|
| array[σ]           | 8σ + flags (216 for a–z) | O(1)      | small, fixed   | small σ, speed matters (DNA, digits, bits) |
| map / unordered_map| ~48 per child         | O(log σ)/O(1)| any            | big or sparse alphabet (Unicode)|
| child/sibling list | 24                    | O(σ)         | any byte       | memory tight, few children      |
| radix (compressed) | fewer nodes overall   | O(1)–O(σ)    | any            | long shared prefixes (paths, IPs)|

`c/03` measures it: 3000 random words → 14 112 nodes → **330 KiB** with
linked children vs **~2976 KiB** with 26-pointer arrays. Same tree, 9× memory.

---

## 3. How it works

### 3.1 Insert — O(L)

Walk the characters; whenever the edge you need is missing, create the node.
Set `is_end` on the last node.

```
 insert "care" into {car, cart}:

    c - a - r* - t*          c - a - r* - t*
                     ===>              \
                                        e*      <- one new node, flag set
```

Optional counter `pass` on every node ("how many words go through me") makes
*"how many words start with P"* an O(P) query: walk P, read `pass`
(`c/01`, `cpp/01`).

### 3.2 Search vs prefix search — O(L)

```
 search(w):       walk w; fail if an edge is missing;  return node->is_end
 starts_with(p):  walk p; fail if an edge is missing;  return true
```

The **only** difference is the final `is_end` check. Forgetting it is the
classic bug: `search("ca")` would wrongly return true.

### 3.3 Delete with pruning — O(L)

Recursively go down, clear `is_end`, then on the way back **up** free every
node that is now useless (no children *and* not the end of another word).
Stop pruning at the first node that is still needed.

```
 delete "cart" from {car, cart, care}:

   c - a - r* - t*            c - a - r* 
               \      ===>              \
                e*                       e*     't' freed; 'r' kept (it is a word
                                                 AND it has child 'e')

 delete "car" afterwards:   'r' loses its flag but still has child 'e' -> kept
 delete "care" afterwards:  'e' freed, 'r' freed, 'a' freed, 'c' freed -> root only
```

`c/01`, `c/03`, `c/04` all assert that deleting every word shrinks the tree
back to the bare root, which proves pruning is complete (and ASan proves no
leaks).

### 3.4 Autocomplete — O(P + subtree)

1. Walk the prefix → node N (O(P)).
2. DFS N's subtree, appending each edge char to a buffer; emit the buffer
   whenever `is_end` is set. Visiting children `a..z` in order emits words
   **sorted**.
3. For "best k suggestions", keep a frequency per word and select the top k
   with a **size-k min-heap**: O(m log k) for m matches (`cpp/02`; heaps are
   lesson 10).

```
 prefix "gs":  root -g-> o -s-> [N]  start DFS here with buf = "gs"
                                  |t*            -> "gst"
                                  +-i-n-s-...*   -> "gstinspect"
                                  +-l-a-u-...*   -> "gstlaunch"
```

Search engines go further and **cache the top-k list in every node**, making
a query O(P + k) at the cost of extra memory and update work.

### 3.5 Compressed trie / radix tree

In a plain trie, a chain of single-child nodes (`r-o-m-a-n-e`) costs one node
per character. A **radix tree** (also *compact trie*; *Patricia tree* is the
binary variant) merges every such chain into **one edge labelled with a
string**.

**Invariant:** every non-root node either ends a word or has ≥ 2 children.
And no two children of a node start with the same character (so children can
be indexed by first char).

**Insert** has three cases. Case C (*split*) is the one to remember:

```
 insert "romulus" into a tree that holds "romane":

   (root)                        (root)
     |"romane"                     |"rom"          <- new middle node at the
    [*]              ===>         [ ]                 divergence point k = 3
                                 /    \
                          "ane" [*]   [*] "ulus"

 A: no child starts with w[0]          -> add a leaf holding all of w
 B: child label is a prefix of w       -> consume it and descend
 C: they share only k < |label| chars  -> split the edge at k (as above);
    if w ends exactly at k, the middle node gets is_end (no new leaf)
```

**Delete** is the mirror: after removing the flag, a node that is not a word
and has **one** child is **merged** with it:

```
     |"om"   (not a word)                 |"oman"
    [ ]                         ===>      [ ]
     |"an"                               /   \
    [ ]                                "e"*  "us"*
   /   \
 "e"*  "us"*
```

`c/04` builds the classic `romane/romanus/romulus/rubens/ruber/rubicon/rubicundus`
example: **14 nodes** instead of 28. `cpp/03` stores sysfs paths: **14 vs 97**.

### 3.6 IPv4 longest-prefix match (binary trie)

A route `10.20.0.0/16` means "the first 16 bits are `00001010 00010100`".
Put routes in a trie over **bits**, MSB first; a `/len` route lives at depth
`len`. To route a packet, walk the destination's bits and remember the
**last** node on the path that carries a route — the deepest one is the
longest prefix.

```
 routes:  0.0.0.0/0 -> A    10.0.0.0/8 -> B    10.20.0.0/16 -> C

 depth 0   [A]                        lookup 10.20.30.41:
            | bits of 10 = 00001010     depth 0 : route A  -> best = A
 depth 8   [B]                          depth 8 : route B  -> best = B
            | bits of 20 = 00010100     depth 16: route C  -> best = C
 depth 16  [C]                          no child for next bit -> stop, answer C
            ...
                                      lookup 10.99.0.1:  A, B, then the
                                      path diverges at depth 9..16 -> B
                                      lookup 8.8.8.8:   only A matches -> A
```

At most **32** steps no matter how many routes — that is why routers love it.
Real routers compress it (Patricia / LC-tries / multibit "stride" tries) so the
walk takes ~4–8 memory accesses instead of 32. Pitfall in C: `~0u << 32` is
undefined behaviour, so the `/0` mask is a special case (`mask_of()` in both
programs).

---

## 4. Complexity

L = key length, P = prefix length, σ = alphabet size, n = number of keys,
N = total number of nodes (≤ sum of key lengths + 1), m = matches.

| Operation                    | Array layout | Map layout     | Linked children | Radix tree | Why |
|------------------------------|--------------|----------------|-----------------|------------|-----|
| insert                       | O(L)         | O(L log σ)     | O(L·σ)          | O(L)       | one step per char; child lookup cost differs |
| search / contains            | O(L)         | O(L log σ)     | O(L·σ)          | O(L)       | same walk, plus `is_end` check |
| count with prefix (`pass`)   | O(P)         | O(P log σ)     | O(P·σ)          | —          | read the counter at the end of the walk |
| list keys with prefix        | O(P + S)     | O(P log σ + S) | O(P·σ + S)      | O(P + S)   | S = size of the subtree under the prefix |
| top-k autocomplete           | O(P + S + m log k) |          |                 |            | DFS + size-k heap |
| delete (with pruning)        | O(L·σ)       | O(L log σ)     | O(L·σ)          | O(L·σ)     | emptiness check scans children |
| IPv4 LPM lookup (binary trie)| O(32)        |                |                 |            | one bit per level |
| **space**                    | O(N·σ)       | O(N)           | O(N)            | O(n) nodes | radix tree: ≤ 2n nodes |

Space is the trie's weakness: worst case every character of every key is a
node. The array layout multiplies that by σ.

---

## 5. C vs C++

| Aspect           | C (`c/`)                                           | C++ (`cpp/`)                                          |
|------------------|----------------------------------------------------|-------------------------------------------------------|
| node children    | `struct Node *child[26]` / sibling list            | `std::array<std::unique_ptr<Node>, σ>` / `std::map<char, unique_ptr<Node>>` |
| freeing          | explicit post-order `trie_free()`                  | automatic: destroying root destroys everything (RAII) |
| pruning a child  | `free(child); parent->kid[i] = NULL;`              | `child.reset();` or `map.erase(it);`                  |
| alphabet         | `#define ALPHA 26`                                 | template parameters `Trie<'a', 26>`, `Trie<'0', 10>`  |
| optional value   | `bool has_route; int hop;`                         | `std::optional<T>`                                    |
| edge label       | `malloc`'d `char *`                                | `std::string`                                         |

**There is no `std::trie`.** What you use in practice:

- `std::set<std::string>` / `std::map`: prefix query = the contiguous range
  starting at `lower_bound(p)` while keys still start with `p`
  (`cpp/01` uses this as its reference). O(log n · L) per step, often good enough.
- A sorted `std::vector<std::string>` + `std::lower_bound`: fastest for static
  dictionaries, cache-friendly.
- Libraries: Boost has none in the core; real projects use e.g. `tsl::htrie`,
  `marisa-trie`, `libart` (adaptive radix tree).

---

## 6. Where it is used in the real world

- **Linux kernel radix tree → XArray.** `lib/radix-tree.c` was a radix tree
  over integer keys (6 bits per level, 64-way nodes); since 4.20 the
  **XArray** (`lib/xarray.c`, `include/linux/xarray.h`) wraps it. The page
  cache maps `(inode, page index) → struct page/folio` with it (`i_pages`),
  IDR/IDA allocate IDs with it. Here the key is an *integer* split into 6-bit
  chunks — same idea as our binary trie with a bigger stride.
- **Linux IPv4 routing (FIB): LC-trie** in `net/ipv4/fib_trie.c` —
  a level- and path-compressed binary trie doing exactly the longest-prefix
  match of `05_ipv4_lpm.c`. `ip route get 10.20.30.41` runs it.
  IPv6 uses a radix tree (`net/ipv6/ip6_fib.c`).
- **Routers / firewalls**: LPM tables in software (DPDK `rte_lpm` uses a
  multibit 24+8 trie) and hardware TCAMs.
- **Databases**: adaptive radix trees (ART) as in-memory indexes (HyPer,
  DuckDB); Redis `rax` (radix tree) backs Streams and cluster key slots.
- **Ethereum** stores state in a *Merkle Patricia trie*.
- **Shells and editors**: bash/zsh completion, IDE symbol completion,
  spell checkers (with edit distance over the trie).
- **Compilers/lexers**: keyword recognisers are tries compiled into
  `switch` statements; Aho–Corasick (multi-pattern search, used by `grep -F`
  and intrusion detection like Snort) is a trie plus failure links.
- **GStreamer / GLib**: caps and element-name lookups use hash tables, but the
  pattern "`gst-inspect-1.0 vid<TAB>`" completion is a classic trie use; and
  sysfs/device-tree path lookups (`/sys/class/...`) are exactly the long
  shared-prefix keys where radix trees shine (`cpp/03`).

---

## 7. Common mistakes and pitfalls

1. **Forgetting `is_end`** — then every prefix "is a word". `search` must
   check the flag; `starts_with` must not.
2. **Deleting a node another word still needs** — prune only when the node
   has *no children and no flag*; stop at the first needed node.
3. **Not pruning at all** — the tree only ever grows; a long-running process
   (route daemon, cache) leaks memory logically even though ASan is happy.
4. **Index out of range**: `c - 'a'` for `'A'`, `'-'`, or a UTF-8 byte is
   negative or ≥ 26 → memory corruption. Validate (assert) or use a map/byte
   alphabet. `char` may be **signed**: cast to `unsigned char` before using a
   byte as an index (`c/03` does).
5. **Memory blow-up of the array layout**: 216 bytes per node × millions of
   nodes. Measure (`c/03`) before choosing.
6. **Radix split off-by-one**: the middle node gets `label[0..k)`, the old
   child keeps `label[k..]` and must be re-keyed under its *new* first char.
7. **Radix delete without merge** — the invariant breaks, nodes with one
   child accumulate, and you have silently built a plain trie again.
8. **Shift by 32 in LPM masks** — `~0u << 32` is UB in C/C++ (x86 actually
   computes `~0u << 0`, giving the wrong mask for `/0`).
9. **Host bits set in a route** — `10.1.2.3/16` must be normalised to
   `10.1.0.0/16` before insert, or the brute-force check and the trie disagree.
10. **Deep recursion** — recursive free/DFS depth = key length. Fine for
    words and IPs; for very long keys (DNA reads, 10⁵ chars) use an explicit
    stack.
11. **`std::map<char,...>` order vs `std::string` order**: `std::less<char>`
    on signed `char` sorts bytes ≥ 0x80 *before* ASCII, `std::string`
    compares as unsigned — DFS order then differs from `std::set` order for
    non-ASCII keys. Use `unsigned char` keys if you need byte order.

---

## 8. Files in this lesson

| File | What it shows | Run |
|------|---------------|-----|
| [c/01_trie_array.c](c/01_trie_array.c) | 26-pointer array trie: insert, search, prefix count (`pass`), delete with pruning; 20 000 random ops vs reference | `make run P=dsa/11-tries/c/01_trie_array.c` |
| [c/02_autocomplete.c](c/02_autocomplete.c) | sorted prefix listing by DFS, frequency-ranked top-k completion | `make run P=dsa/11-tries/c/02_autocomplete.c` |
| [c/03_trie_linked_children.c](c/03_trie_linked_children.c) | first-child/next-sibling layout, any byte (UTF-8), pointer-to-pointer insert, memory measured vs array layout | `make run P=dsa/11-tries/c/03_trie_linked_children.c` |
| [c/04_radix_tree.c](c/04_radix_tree.c) | compressed trie: edge split on insert, merge on delete, invariant checked | `make run P=dsa/11-tries/c/04_radix_tree.c` |
| [c/05_ipv4_lpm.c](c/05_ipv4_lpm.c) | binary trie routing table, longest-prefix match vs brute-force scan (150 000 lookups) | `make run P=dsa/11-tries/c/05_ipv4_lpm.c` |
| [cpp/01_trie_unique_ptr.cpp](cpp/01_trie_unique_ptr.cpp) | `Trie<First, Alpha>` template with `unique_ptr` children, any alphabet; checked vs `std::set` prefix ranges | `make run P=dsa/11-tries/cpp/01_trie_unique_ptr.cpp` |
| [cpp/02_trie_map_autocomplete.cpp](cpp/02_trie_map_autocomplete.cpp) | `std::map` children, autocomplete with size-k min-heap (`std::priority_queue`) | `make run P=dsa/11-tries/cpp/02_trie_map_autocomplete.cpp` |
| [cpp/03_radix_tree.cpp](cpp/03_radix_tree.cpp) | radix tree with `std::string` edges on sysfs paths, node count vs plain trie | `make run P=dsa/11-tries/cpp/03_radix_tree.cpp` |
| [cpp/04_ipv4_lpm.cpp](cpp/04_ipv4_lpm.cpp) | `LpmTable<T>` with `std::optional`, CIDR parsing, route listing, erase | `make run P=dsa/11-tries/cpp/04_ipv4_lpm.cpp` |

---

## 9. Exercises

1. **(easy)** Add `longest_common_prefix()` of all stored words: walk from the
   root while the node has exactly one child and is not a word end.
   *Hint:* that is just the radix tree's first edge.
2. **(easy)** Make `c/01` case-insensitive by mapping `A–Z` to `a–z` in `idx()`.
3. **(medium)** "Replace words": given roots `{cat, bat, rat}`, turn
   `"the cattle was rattled"` into `"the cat was rat"`. *Hint:* walk each
   word and stop at the first `is_end` — shortest stored prefix.
4. **(medium)** Word search with wildcards: `search("c.t")` where `.` matches
   any letter. *Hint:* DFS branching into all children at a `.`.
5. **(medium)** Cache the top-3 completions in every node of `cpp/02` so
   `top_k` is O(P + k). Update the caches along the path on every `add`.
6. **(hard)** Turn `c/05` into a **path-compressed** binary trie (store a
   skip count / bit string per edge, like `fib_trie.c`) and count nodes for
   the same random table.
7. **(hard)** Multibit trie: stride 8 (`child[256]`) for IPv4 so a lookup is
   at most 4 steps. Handle prefixes that are not a multiple of 8 with
   *controlled prefix expansion* (a /12 becomes 16 /16 entries).
8. **(hard)** Implement Aho–Corasick on top of `c/01`: BFS to compute failure
   links, then find all occurrences of many patterns in a text in one pass.

---

## 10. Remember forever

```
TRIE = keys are PATHS; one edge per char (or bit); flag marks "word ends here".
insert/search = walk L chars: O(L), independent of n. search checks is_end.
prefix query  = walk P, then DFS the subtree (children in order => sorted).
delete        = clear flag, then free upward while node has no kids & no flag.
layouts       = array[σ] fast/fat | map any-alphabet | child/sibling lean/slow.
RADIX         = compress single-child chains into string edges;
                insert SPLITS at divergence k, delete MERGES lonely node + child.
LPM (routing) = binary trie on address bits; walk ≤32 bits; deepest route wins.
Linux: XArray/radix tree = page cache; fib_trie (LC-trie) = IPv4 routes.
```

Review it with the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Why does a trie lookup not depend on the number of stored keys? What does it depend on?
2. What exactly differs between `search("ca")` and `starts_with("ca")`, in code?
3. You delete `"cart"` from `{car, cart, care}`. Which node(s) are freed, and why is `r` kept?
4. Why does a DFS over a trie produce keys in sorted order? When does that break (hint: `char` signedness)?
5. Compare memory of the array, map and first-child/next-sibling layouts. When is each the right choice?
6. State the radix-tree invariant. Draw the split when inserting `"romulus"` next to `"romane"`.
7. After a delete in a radix tree, when must you merge, and with what?
8. Walk the lookup of `10.20.30.41` against `{0/0, 10/8, 10.20/16}`. Why is the *last* route on the path the answer?
9. Why is `~0u << (32 - len)` wrong for `len = 0`, and what does x86 actually do?
10. What do Linux's XArray and `fib_trie.c` store, and what are their keys?
11. How would you get the top-k completions in O(P + k) per query?
