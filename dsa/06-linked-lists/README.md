# 06 — Linked lists

> Arrays put elements **next to each other**. Linked lists put them
> **anywhere** and connect them with **pointers**. Everything in this lesson
> follows from that one choice.

---

## 1. Why this exists

An array is one contiguous block. That gives fast indexing (`a[i]` is one
address calculation), but it has two costs:

- **Inserting or deleting in the middle shifts everything after it**: O(n).
- **Growing means reallocating and copying**, and every pointer into the old
  block becomes invalid.

A linked list removes both costs. Each element lives in its own small heap
block (a **node**) that also stores the address of the next node. To insert,
you allocate a node and change **two pointers**. Nothing moves, so a pointer
to a node stays valid for as long as the node exists.

The price is that you lose random access (to reach element 1000 you follow
1000 pointers) and cache friendliness (nodes are scattered in memory).

Use a list when **you already hold a pointer to the place you want to change**
(a task in a scheduler, a buffer in a driver queue, an entry in an LRU cache)
and you need O(1) insert or remove there. Don't use one just to store a
sequence: `std::vector` is almost always faster for that.

---

## 2. Mental model

**A treasure hunt.** Each clue (node) holds a prize (the value) and the
location of the next clue (the `next` pointer). You can only find clue 5 by
following clues 1 to 4. To add a clue in the middle, you write a new card and
change what one existing card points to. No other card moves.

```
 Array (contiguous)                Singly linked list (scattered)

 addr 0x100 0x104 0x108 0x10c       head
      +----+----+----+----+          |
      | 10 | 20 | 30 | 40 |          v
      +----+----+----+----+        0x7f30            0x5a10            0x6c88
                                   +----+------+     +----+------+     +----+------+
 a[2] = *(base + 2*4)  O(1)        | 10 |0x5a10|---->| 20 |0x6c88|---->| 30 | NULL |
                                   +----+------+     +----+------+     +----+------+
                                    value next
                                   reaching index 2 = follow 2 pointers  O(n)
```

The three shapes in this lesson:

```
 singly:    head -> [A] -> [B] -> [C] -> NULL

 doubly:    NULL <- [A] <=> [B] <=> [C] -> NULL
            head                     tail

 circular:  [A] -> [B] -> [C] --+          (keep `tail`; head = tail->next)
             ^                  |
             +------------------+
```

---

## 3. How it works

### 3.1 Singly linked list

```c
typedef struct Node { int value; struct Node *next; } Node;
typedef struct { Node *head, *tail; size_t size; } List;
```

The list is just a pointer to the first node. We also cache `tail` (so
`push_back` is O(1)) and `size` (so `length()` is O(1)).

**push_front: O(1).** Order matters: link the new node first, then move head.

```
 before:  head -> [A] -> [B] -> NULL

 1. n->next = head          [N] -> [A]
 2. head = n        head -> [N] -> [A] -> [B] -> NULL

 Swap the two steps and you get head -> [N] -> NULL: the old chain is lost (leak).
```

**Insert after a node `prev`: O(1) once you hold `prev`** (O(pos) to walk there).

```
 head -> [A] -> [B] -> [C]            prev = [B]

 1. n->next = prev->next              [N] -> [C]
 2. prev->next = n                    [B] -> [N]

 head -> [A] -> [B] -> [N] -> [C]
```

**Delete the node after `prev`: O(1)** (but finding `prev` is O(n)).

```
 head -> [A] -> [B] -> [C]            prev = [A], victim = [B]

 prev->next = victim->next            [A] ---------> [C]
 free(victim)
```

**pop_back is O(n) even with a tail pointer.** To remove the tail you must set
the *previous* node's `next` to NULL, and a singly list cannot go backwards.
That single fact is the main reason doubly linked lists exist.

**Freeing: save `next` before `free`.**

```c
while (n) { Node *next = n->next; free(n); n = next; }   /* right */
for (n = head; n; n = n->next) free(n);                  /* use-after-free! */
```

### 3.2 Doubly linked list

Each node has `prev` and `next`. That extra 8 bytes per node buys:

- `pop_back` in O(1).
- **Unlink a node you hold in O(1)**: its neighbours are `x->prev` and
  `x->next`, so there is nothing to search for.
- Walking backwards (and walking from whichever end is closer to an index).

Insert is four pointer writes. **Wire the new node first, then the neighbours:**

```
 before:   [P] <=====> [Q]          insert N between P and Q

 1. n->prev = P                     these two only touch N,
 2. n->next = Q                     so they cannot break anything
 3. Q->prev = n
 4. P->next = n

 after:    [P] <=> [N] <=> [Q]
```

Unlink is two writes:

```
 [P] <=> [X] <=> [Q]

 X->prev->next = X->next            P -> Q
 X->next->prev = X->prev            P <- Q
 free(X)
```

With plain NULL-terminated ends, each of those lines needs an
`if (x->prev) ... else head = ...` branch. The **sentinel** trick removes them.

### 3.3 Sentinel (dummy) nodes: deleting the special cases

A sentinel is a node that holds no data and is always present. Make the
doubly list **circular around a sentinel** and every real node always has
a real `prev` and `next`:

```
 empty:    +----------+               3 items:
           v          |               [S] <=> [a] <=> [b] <=> [c] <=> back to [S]
          [S] --------+               end()   begin()                 --end()
          (prev and next point at S)
```

Now insert is always "link before pos" and erase is always "unlink x", with no
`if` at all. This is exactly how `std::list` and the Linux kernel's
`struct list_head` work.

For singly lists, a **dummy node on the stack** does the same job for
algorithms that build a new list (merge) or may remove the head (remove n-th
from end):

```c
Node dummy = {0, head};      /* the "node before the head" */
... work with &dummy as prev ...
return dummy.next;           /* the real (possibly new) head */
```

### 3.4 Circular list

The last node points back to the first. **Keep a pointer to the tail**, not the
head: the head is `tail->next`, so you get both ends from one pointer.

```
 push_front:  n->next = tail->next; tail->next = n;        O(1)
 push_back:   push_front, then tail = tail->next;          O(1)
 rotate:      tail = tail->next;                           O(1)  round-robin step
```

Loops cannot stop at NULL (there is none). Use `do { ... } while (n != head)`.
Uses: round-robin scheduling, the Josephus problem, and ring-shaped buffer
lists such as DMA descriptor rings (the hardware follows `next` forever).

### 3.5 The pointer-to-pointer trick

The usual delete has two cases:

```c
if (prev == NULL) head = cur->next;      /* deleting the head    */
else              prev->next = cur->next; /* deleting anything else */
```

Both branches overwrite **the pointer that points at `cur`**. So keep a pointer
to that pointer:

```c
Node **link = &head;                     /* the slot that points at the candidate */
while (*link && (*link)->value != v)
    link = &(*link)->next;               /* move to the next slot */
if (*link) { Node *x = *link; *link = x->next; free(x); }
```

```
 head --> [3] --> [7] --> [9] --> NULL
  ^          ^
  |          |
 link       link (after one step)       = &node3->next
 = &head

 Remove [7] when link == &node3->next:   *link = (*link)->next
 head --> [3] -------------> [9] --> NULL
```

When the victim is the head, `link == &head` and the same line updates head.
There is one case, not two. Linus Torvalds used this as his example of "good
taste" in code. The same trick does sorted insert and remove-all-matching in
one pass (see `07_pointer_to_pointer.c`). `std::forward_list::before_begin()`
is the STL version of the same idea.

### 3.6 Reverse

**Iterative: O(n) time, O(1) space.** Three pointers; flip one arrow per step.

```
 prev = NULL, cur = head
 loop:  next = cur->next;   cur->next = prev;   prev = cur;   cur = next;

 start:    NULL    [1] -> [2] -> [3] -> NULL
 step 1:   NULL <- [1]    [2] -> [3] -> NULL      prev=[1]  cur=[2]
 step 2:   NULL <- [1] <- [2]    [3] -> NULL      prev=[2]  cur=[3]
 step 3:   NULL <- [1] <- [2] <- [3]              prev=[3]  cur=NULL
 return prev
```

Mnemonic: **save, flip, step, step** (`next = ...; cur->next = prev; prev = cur; cur = next;`).

**Recursive: O(n) time, O(n) stack.** Reverse the rest, then hang yourself
after your old successor (which is now the tail of the reversed rest):

```c
Node *rev(Node *h) {
    if (!h || !h->next) return h;
    Node *new_head = rev(h->next);
    h->next->next = h;          /* my old next now points back at me */
    h->next = NULL;             /* I am the new tail                */
    return new_head;
}
```

One stack frame per node: a million-node list can overflow the stack. Prefer
the iterative version in real code.

### 3.7 Slow and fast pointers

`slow` moves 1, `fast` moves 2. When `fast` reaches the end, `slow` is halfway.

```
 while (fast && fast->next) { slow = slow->next; fast = fast->next->next; }

 5 nodes: [1] [2] [3] [4] [5]   slow stops on [3]
 4 nodes: [1] [2] [3] [4]       slow stops on [3]  (second middle)

 first middle (for splitting in merge sort):
 while (fast->next && fast->next->next) ...      4 nodes -> [2]
```

### 3.8 Floyd cycle detection (tortoise and hare)

If there is a cycle, `fast` never reaches NULL. Inside the loop `fast` gains
one node per step on `slow`, so the gap shrinks by one every step and they
**must** meet. If `fast` hits NULL, there is no cycle. O(n) time, O(1) space.

**Finding where the cycle starts:** after they meet, put one pointer back at
the head and move both one step at a time. They meet at the start.

```
 head ---- a ----> S ---- b ----> M        S = cycle start
                   ^              |        M = meeting point
                   +----- c ------+        loop length L = b + c

 slow walked a + b.  fast walked a + b + kL (k whole extra laps).
 fast walked twice as far:  2(a + b) = a + b + kL   =>   a = kL - b = (k-1)L + c

 So a steps from head reaches S, and a steps from M (c steps, then whole
 laps) also reaches S. Walk both 1 step at a time: they collide at S.
```

Loop length: from M, walk once round until you return to M.

### 3.9 Merge two sorted lists

Relink the existing nodes; don't copy. A dummy node (or a `Node **link`)
removes the "first node" special case.

```
 dummy -> ?           tail = &dummy
 a: [1] -> [4]        b: [2] -> [3]

 take 1 (a):  dummy -> [1]
 take 2 (b):  dummy -> [1] -> [2]
 take 3 (b):  dummy -> [1] -> [2] -> [3]
 b empty:     tail->next = a   (attach the whole rest [4] in ONE link)
```

Use `<=` so that on ties you take from `a` first: the merge stays **stable**.
Merge + slow/fast split = **merge sort**, the standard sort for linked lists
(O(n log n), needs no random access). `std::list::sort` does exactly this.

### 3.10 Remove the n-th node from the end

Two pointers with a gap of `n`. Start both at a dummy node, move `lead`
`n+1` steps, then move both until `lead` is NULL. `lag` is now just before the
victim. One pass, O(1) space, and removing the head needs no special case.

```
 remove 2nd from end of 1 2 3 4 5:

 dummy  1  2  3  4  5  NULL
 lag          lead                 (lead moved n+1 = 3 steps)
          ...both advance...
              lag      lead=NULL   lag = [3], victim = [4]
 lag->next = lag->next->next
```

### 3.11 Linux kernel `list_head` and `container_of`

A normal list node **contains** your data. The kernel turns that around: your
data **contains** the list node. This is called an **intrusive** list.

```c
struct list_head { struct list_head *next, *prev; };

struct task {
    int pid;
    char name[16];
    struct list_head all;     /* on the list of all tasks */
    struct list_head queue;   /* on the run queue OR a wait queue */
};
```

The list code only sees `struct list_head`. To get from a `list_head *` back
to the `struct task` around it, use `container_of`:

```c
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
```

```
 struct task at 0x1000
 +-------------------+ 0x1000   <- container_of(p, struct task, queue)
 | pid, name[16]     |             = 0x1028 - offsetof(struct task, queue)
 | list_head all     | 0x1018      = 0x1028 - 0x28 = 0x1000
 | list_head queue   | 0x1028   <- p
 +-------------------+
```

Why the kernel does it this way:

- **No allocation to insert.** The node is already inside the object.
  Allocation in interrupt context, or with locks held, is restricted, so this
  matters a lot.
- **One object, many lists.** Add one `list_head` member per list.
- **One generic implementation** for every type: no templates in C, no `void *`
  casts, no extra pointer to follow.
- **Circular with a sentinel head**, so `list_add`, `list_add_tail` and
  `list_del` are 2–4 pointer writes with zero branches.

`list_for_each_entry_safe` keeps a look-ahead pointer, so you can `list_del`
and `free` the current entry inside the loop. The non-safe version would read
`pos->member.next` after the free: a use-after-free.

You will meet `list_head` everywhere in the kernel: the tasks of a process
(`task_struct.tasks`), `struct device` children, V4L2 buffer queues
(`vb2_buffer.queued_entry`, the `done_list`), the DMA engine's descriptor
lists, timers, and the workqueue. `hlist_head` (one pointer, used for
hash-table buckets) is the variant covered in the hashing lesson.

---

## 4. Complexity table

| Operation                       | Singly (head+tail)  | Doubly            | Why |
|---------------------------------|---------------------|-------------------|-----|
| access k-th element             | O(k)                | O(min(k, n−k))    | must follow pointers; doubly can start from the closer end |
| search by value                 | O(n)                | O(n)              | no order, no index |
| push_front / pop_front          | O(1)                | O(1)              | only head changes |
| push_back                       | O(1)                | O(1)              | thanks to cached tail |
| pop_back                        | **O(n)**            | O(1)              | singly must find the node before tail |
| insert after a node you hold    | O(1)                | O(1)              | rewire 2 (or 4) pointers |
| delete a node you hold          | O(n) (find prev)    | **O(1)**          | doubly knows its prev |
| length                          | O(1) if cached      | O(1) if cached    | else O(n) walk |
| reverse                         | O(n), O(1) space    | O(n), O(1) space  | flip each arrow once |
| middle (slow/fast)              | O(n), O(1) space    | same              | one pass |
| detect cycle (Floyd)            | O(n), O(1) space    | same              | fast gains 1 per step |
| merge two sorted (n, m)         | O(n+m), O(1) space  | same              | relink, no copies |
| merge sort                      | O(n log n), O(log n) stack | same       | log n levels, O(n) merge each |
| memory per element              | value + 1 pointer + malloc header | + 1 more pointer | plus scattered nodes = cache misses |

**The hidden constant:** walking a list is a chain of *dependent loads*. The
CPU cannot fetch node k+1 until node k arrives, so each step can cost a cache
miss (~100 ns on a desktop, worse on a Cortex-A72). An array scan is
prefetched and often 10–100× faster for the same O(n).

---

## 5. C vs C++

| Concept              | C (this lesson)                         | C++ (this lesson)                                    | Standard library |
|----------------------|-----------------------------------------|------------------------------------------------------|------------------|
| singly list          | `Node *head` + manual `malloc/free`     | `SinglyList<T>` with `std::unique_ptr<Node> next`    | `std::forward_list<T>` |
| doubly list          | `prev`/`next` + head/tail branches      | `DList<T>` with a sentinel, bidirectional iterators  | `std::list<T>` |
| intrusive list       | `struct list_head` + `container_of`     | `ListHook<Tag>` base class + `static_cast`           | `boost::intrusive::list` |
| "node before head"   | `Node **link` or a dummy node            | `std::unique_ptr<Node> *link`                        | `forward_list::before_begin()` |
| cleanup              | walk and `free`, saving `next` first    | destructor (RAII)                                    | automatic |

Things C++ adds:

- **RAII.** The destructor frees every node; an early `return` or an exception
  cannot leak.
- **`unique_ptr` makes ownership visible**: `head_` owns node 1, which owns
  node 2. The trap is that the *default* destructor then frees the chain
  **recursively**, one stack frame per node, so a long list overflows the
  stack. `01_singly_list_template.cpp` writes an iterative `clear()`.
- **A doubly list can't use `unique_ptr` for both links** (two owners). Use
  raw pointers with the *list* as owner (as `std::list` does), or
  `unique_ptr next` plus raw `prev`.
- **Iterators** let your list work with range-for and `<algorithm>`
  (`std::find`, `std::distance`, `std::reverse_iterator`...).
- **Moving a list with an embedded sentinel** must re-point the first and last
  nodes at the new sentinel address (see `DList::swap`).

The `std::` lists, the parts you will use:

```cpp
std::list<int> l;
l.push_back(x); l.push_front(x); l.insert(it, x); it = l.erase(it);
l.splice(pos, other, first, last);   // O(1) move of nodes, no copies
l.sort(); l.merge(other); l.unique(); l.remove_if(pred); l.reverse();
// std::sort(l.begin(), l.end()) does NOT compile: needs random access.

std::forward_list<int> f;            // 8 bytes on 64-bit (one pointer); no size()
f.push_front(x); f.insert_after(it, x); f.erase_after(it); f.before_begin();
```

**Iterator stability** is the key guarantee: insert and erase on a list never
invalidate iterators to *other* elements. With `std::vector`, both can.
This is what makes the **LRU cache** possible: a `std::list` keeps the usage
order and an `unordered_map<Key, list::iterator>` finds a node in O(1). On a
hit, `splice` moves the node to the front in O(1) (see
`03_std_list_forward_list.cpp`).

**Rule of thumb:** use `std::vector` by default. Use `std::list` when you need
stable iterators, O(1) splice, or O(1) removal through a handle you already
hold. Use an intrusive list when objects must sit on several lists, or when
you must not allocate.

---

## 6. Where it is used in the real world

- **Linux kernel**: `struct list_head` is everywhere: the process list,
  run-queue entries, device trees of `struct device`, the module list, timer
  wheels, and the waiters on every wait queue (`wait_queue_head_t`).
  `hlist_head` (singly-headed) chains hash buckets (PID hash, inode and dentry
  caches). `llist` is a lock-free singly list for producers in IRQ context.
- **V4L2 / videobuf2** (camera capture on the TDA4VM): queued buffers sit on
  `q->queued_list` and finished ones on `q->done_list`, both `list_head`.
  `VIDIOC_QBUF` adds to a list and `VIDIOC_DQBUF` takes from one.
- **DMA descriptor chains**: hardware DMA engines (including TI's UDMA/BCDMA
  on K3 SoCs) follow linked descriptors in memory. Each descriptor holds
  the *physical* address of the next one, so the "list" is walked by hardware.
  Circular chains give continuous capture.
- **Networking**: `struct sk_buff` queues (`sk_buff_head`) are doubly linked
  lists of packets; fragments are chained via `frag_list`.
- **Memory allocators**: `malloc` free lists (glibc bins, embedded allocators
  like FreeRTOS `heap_4`), and the kernel's buddy allocator free lists of pages.
- **GStreamer / GLib**: `GList` and `GSList` (doubly/singly) are used for pad
  lists, element children in a `GstBin` and the caps structure list;
  `GQueue` is a doubly linked list with head/tail/length.
- **Caches**: LRU = hash map + doubly linked list (Linux page cache LRU lists,
  memcached, Redis eviction pools, CPU-side caches in databases).
- **Editors and undo**: text editors' piece tables, undo/redo stacks.
- **RTOS schedulers**: FreeRTOS keeps ready tasks per priority in circular
  doubly linked lists (`xList`) and rotates them for round-robin.

---

## 7. Common mistakes and pitfalls

1. **Losing the rest of the list** by overwriting a `next` before saving it.
   Always: save `next`, then rewire.
2. **Use-after-free when freeing**: `for (n = head; n; n = n->next) free(n);`
   reads `n->next` after `free(n)`. Save `next` first. ASan reports it.
3. **Stale `tail`**: deleting the last node without updating `tail`, and the
   next `push_back` writes into freed memory.
4. **Forgetting `prev` in a doubly list**: the forward walk looks fine, the
   backward walk is broken. Self-check both directions (`equals()` in
   `02_doubly_linked_list.c`).
5. **Head special case bugs**: deleting the head, inserting before the head,
   or a list with one node. Use a sentinel, a dummy node, or `Node **link`.
6. **Dereferencing NULL in slow/fast loops**: the condition must be
   `fast && fast->next` (check `fast` first; `&&` short-circuits).
7. **Freeing a list that has a cycle**: "walk until NULL" never ends, or
   double-frees. Break the cycle first.
8. **Comparing values instead of addresses** in Floyd: two different nodes
   may hold the same value. Compare pointers.
9. **Infinite loop in a circular list**: `while (n)` never ends there. Use
   `do { } while (n != head)`.
10. **Recursion depth**: recursive reverse, recursive free, or a `unique_ptr`
    chain's implicit destructor: one stack frame per node. Long list, stack
    overflow.
11. **Iterating while deleting in a kernel-style list**: use
    `list_for_each_entry_safe`.
12. **Using a list when a vector would do**: O(1) insert does not help if you
    first have to walk O(n) to find the spot, and cache misses dominate.
13. **`assert()` with side effects**: `assert(pop_front(&l, &v))` disappears
    with `-DNDEBUG`, and so does the pop. These programs use a `CHECK` macro
    that always evaluates its argument.

---

## 8. Files in this lesson

| File | What it shows | Run |
|------|---------------|-----|
| [c/01_singly_linked_list.c](c/01_singly_linked_list.c) | head+tail+size list: push/pop front/back, insert/remove at position, search, free | `make run P=dsa/06-linked-lists/c/01_singly_linked_list.c` |
| [c/02_doubly_linked_list.c](c/02_doubly_linked_list.c) | prev/next, O(1) pop_back and unlink-this-node, walk from the nearer end | `make run P=dsa/06-linked-lists/c/02_doubly_linked_list.c` |
| [c/03_circular_list.c](c/03_circular_list.c) | tail-only circular list, round-robin rotation, Josephus problem | `make run P=dsa/06-linked-lists/c/03_circular_list.c` |
| [c/04_reverse_and_middle.c](c/04_reverse_and_middle.c) | reverse iterative and recursive, middle (first/second), palindrome in O(1) space | `make run P=dsa/06-linked-lists/c/04_reverse_and_middle.c` |
| [c/05_floyd_cycle.c](c/05_floyd_cycle.c) | detect cycle, find its start, measure its length, break it; exhaustive test | `make run P=dsa/06-linked-lists/c/05_floyd_cycle.c` |
| [c/06_merge_and_remove_nth.c](c/06_merge_and_remove_nth.c) | merge two sorted lists (dummy node), remove n-th from end, merge sort | `make run P=dsa/06-linked-lists/c/06_merge_and_remove_nth.c` |
| [c/07_pointer_to_pointer.c](c/07_pointer_to_pointer.c) | `Node **link`: delete, sorted insert, remove_if with no head special case | `make run P=dsa/06-linked-lists/c/07_pointer_to_pointer.c` |
| [c/08_kernel_list_head.c](c/08_kernel_list_head.c) | mini `list.h`: `list_head`, `container_of`, add/del/move, `_safe` iteration, one task on two lists | `make run P=dsa/06-linked-lists/c/08_kernel_list_head.c` |
| [cpp/01_singly_list_template.cpp](cpp/01_singly_list_template.cpp) | `SinglyList<T>` with `unique_ptr`, iterators, rule of five, iterative destructor | `make run P=dsa/06-linked-lists/cpp/01_singly_list_template.cpp` |
| [cpp/02_doubly_list_template.cpp](cpp/02_doubly_list_template.cpp) | `DList<T>`: sentinel, bidirectional iterators, emplace/erase/splice/reverse — a mini `std::list` | `make run P=dsa/06-linked-lists/cpp/02_doubly_list_template.cpp` |
| [cpp/03_std_list_forward_list.cpp](cpp/03_std_list_forward_list.cpp) | `std::list` and `std::forward_list` API, iterator stability, splice, LRU cache preview | `make run P=dsa/06-linked-lists/cpp/03_std_list_forward_list.cpp` |
| [cpp/04_list_algorithms.cpp](cpp/04_list_algorithms.cpp) | the classics as templates over a node pool; reorder list; random cross-check vs `std::list` | `make run P=dsa/06-linked-lists/cpp/04_list_algorithms.cpp` |
| [cpp/05_intrusive_list.cpp](cpp/05_intrusive_list.cpp) | type-safe intrusive list with tagged base-class hooks and auto-unlink on destruction | `make run P=dsa/06-linked-lists/cpp/05_intrusive_list.cpp` |

---

## 9. Exercises

**Easy**
1. Add `list_get(const List *l, size_t i, int *out)` to `01_singly_linked_list.c`.
   *Hint:* bounds-check against `size` first.
2. Write `count_occurrences(head, value)` and `sum(head)` recursively and
   iteratively.
3. Print a singly list in reverse **without** modifying it.
   *Hint:* recursion prints on the way back up (or use an explicit stack).

**Medium**
4. Remove duplicates from a **sorted** list in one pass, then from an
   **unsorted** one. *Hint:* sorted means compare neighbours; unsorted needs a
   hash set (lesson 08) or O(n²).
5. Find the intersection node of two singly lists that merge into a Y shape.
   *Hint:* walk A then B, and B then A; both pointers travel `a + b + c`.
6. Rotate a list right by k. *Hint:* make it circular, then cut at `n - k % n`.
7. Add two numbers stored as digit lists (least significant digit first).
8. Add `list_splice_tail(list, head)` to the mini `list.h` in
   `08_kernel_list_head.c` (move a whole list in O(1)).

**Hard**
9. Reverse a list in groups of k (`1 2 3 4 5`, k=2 gives `2 1 4 3 5`).
   *Hint:* reverse k nodes, recurse on the rest, link them.
10. Copy a list where each node also has a `random` pointer to any node.
    *Hint:* interleave copies (`A A' B B'`), set `A'->random = A->random->next`,
    then split. O(1) extra space.
11. Merge k sorted lists in O(N log k). *Hint:* min-heap of list heads (lesson
    10), or pairwise merging like merge sort.
12. Implement a **skip list** (lesson 12 preview): a sorted linked list with
    express lanes, giving O(log n) expected search.
13. Make the C++ `SinglyList<T>` `const`-correct with a `const_iterator` that
    converts from `iterator`, and add `erase_after`.

---

## 10. Remember forever

```
NODE        value + next (+ prev).  The list is just a pointer to the first node.
INSERT      wire the NEW node first, then the old neighbours.   (save -> rewire)
DELETE      need the node BEFORE (singly) or just the node (doubly). Save next, then free.
REVERSE     save, flip, step, step:  next=c->next; c->next=p; p=c; c=next;  return p.
SLOW/FAST   1 and 2 steps: middle when fast ends; meet => cycle;
            reset one to head, both step 1 => meet at cycle start.
NO SPECIAL  dummy node, sentinel, or Node **link = &head  =>  no "is it the head?" if.
KERNEL      data CONTAINS the list_head; container_of = (char*)ptr - offsetof(type, member).
COST        O(1) splice/unlink when you hold the node; O(n) to find it; cache-unfriendly.
```

Review on the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Why is `pop_back` O(n) on a singly list even when you keep a tail pointer?
2. Draw the four pointer writes for inserting into a doubly list. Which two
   are safe to do first, and why?
3. What does a sentinel node buy you? Where does `std::list::end()` point?
4. Explain the pointer-to-pointer delete. What is `link` at the start, and
   what does `*link = victim->next` change when the victim is the head?
5. Reverse a 3-node list on paper, writing `prev`, `cur` and `next` after each
   step.
6. Why must slow and fast meet if there is a cycle? Prove that resetting one
   pointer to the head finds the cycle's start.
7. Why does merge sort suit linked lists, while quicksort and heapsort don't?
8. What does `container_of(ptr, struct task, queue)` compute? Why can one
   `struct task` sit on two lists at once?
9. Why does the kernel need `list_for_each_entry_safe`? What goes wrong
   without it?
10. Why might a `std::unique_ptr`-linked list crash with a stack overflow in its
    destructor, and how do you fix it?
11. When does a linked list beat `std::vector` in practice, and when does it
    lose even though its big-O looks better?
