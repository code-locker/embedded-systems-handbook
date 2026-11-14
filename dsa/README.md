# DSA track: data structures and algorithms in C and C++

Why both languages? **C shows you what really happens.** Every pointer,
allocation and byte is visible. **C++ shows how you would write it in
production**, with RAII, templates and the STL. Each lesson implements the
idea by hand in both, then shows the standard-library equivalent.

## Lessons

| # | Lesson | Key ideas |
|---|---|---|
| 00 | [Foundations](00-foundations/README.md) | compile → assemble → link, types, integer rules, undefined behaviour, C vs C++ |
| 01 | [Pointers](01-pointers/README.md) | addresses, arithmetic, arrays vs pointers, `**`, `void*`, function pointers, references |
| 02 | [Memory management](02-memory-management/README.md) | process layout, stack frames, heap, allocators, alignment, pools, RAII, smart pointers, move |
| 03 | [Debugging](03-debugging/README.md) | gdb, valgrind, ASan/UBSan, core dumps, strace; programs with planted bugs to hunt |
| 04 | [Complexity analysis](04-complexity-analysis/README.md) | Big-O, amortised cost, Master theorem, why the cache matters |
| 05 | [Arrays and strings](05-arrays-and-strings/README.md) | dynamic array, two pointers, sliding window, prefix sums, KMP |
| 06 | [Linked lists](06-linked-lists/README.md) | singly/doubly/circular, Floyd's cycle detection, kernel `list_head` |
| 07 | [Stacks and queues](07-stacks-and-queues/README.md) | ring buffer, deque, monotonic stack, lock-free SPSC queue |
| 08 | [Hashing](08-hashing/README.md) | hash functions, chaining, open addressing, Robin Hood, Bloom filter |
| 09 | [Trees](09-trees/README.md) | traversals, BST, AVL, red-black |
| 10 | [Heaps and priority queues](10-heaps-priority-queues/README.md) | sift-up/down, O(n) build-heap, top-k, running median, timers |
| 11 | [Tries](11-tries/README.md) | prefix search, autocomplete, radix tree, IP longest-prefix match |
| 12 | [Advanced trees](12-advanced-trees/README.md) | segment tree, Fenwick, B-tree/B+ tree, skip list |
| 13 | [Graphs](13-graphs/README.md) | types, representations, BFS/DFS, shortest paths, MST, SCC, flow, where used |
| 14 | [Sorting and searching](14-sorting-and-searching/README.md) | all the main sorts, stability, introsort, binary-search variants |
| 15 | [Recursion and backtracking](15-recursion-and-backtracking/README.md) | call stack, recursion trees, N-Queens, Sudoku |
| 16 | [Greedy](16-greedy/README.md) | exchange argument, intervals, Huffman coding |
| 17 | [Dynamic programming](17-dynamic-programming/README.md) | memo vs table, knapsack, LIS, LCS, edit distance, bitmask DP |
| 18 | [Bit manipulation](18-bit-manipulation/README.md) | masks, register read-modify-write, popcount, endianness |
| 19 | [System-design structures](19-system-design-structures/README.md) | LRU/LFU cache, object pool, token bucket, timer wheel |

Follow the order in [docs/ROADMAP.md](../docs/ROADMAP.md).

## Inside each lesson

```
NN-topic/
├── README.md   theory → diagrams → complexity → real world → pitfalls → exercises → recall card
├── c/          hand-written C implementations
└── cpp/        modern C++ implementations + STL equivalents
```

Run a program with `make run P=dsa/<lesson>/c/<file>.c` from the repo root.

## The one-page "big picture"

```
               need fast lookup by key? ──yes──▶ hash map  (O(1) avg)
                          │no                      └─ need ordering too? ─▶ balanced BST / std::map
                          ▼
           need min/max repeatedly? ──yes──▶ heap / priority queue
                          │no
                          ▼
      need prefix queries on strings? ─yes─▶ trie
                          │no
                          ▼
  range queries + updates on an array? ─yes─▶ Fenwick / segment tree
                          │no
                          ▼
    things connected to other things? ─yes─▶ graph (BFS/DFS/Dijkstra/...)
                          │no
                          ▼
     order of insertion/removal matters ─▶ stack (LIFO) / queue (FIFO) / deque
                          │
                          ▼
                 default: dynamic array (std::vector), which is cache-friendly and usually fastest
```
