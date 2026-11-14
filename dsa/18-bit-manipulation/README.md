# 18 — Bit manipulation

> Every register you poke on the TDA4VM, every pixel format flag in GStreamer,
> every permission bit in `ls -l` is a handful of bits packed into an integer.
> This lesson makes those bits feel like ordinary objects you can pick up,
> move, and inspect.

---

## 1. Why this exists

A computer stores everything as bits, but most code pretends it doesn't.
You need to stop pretending when:

- **Talking to hardware.** A peripheral register is 32 bits holding 5–10
  separate fields. Changing one field without wrecking the others is a
  bit-manipulation problem (read-modify-write).
- **Saving memory.** 1 million booleans as `bool[]` = 1 MB; as a bitset =
  125 KB, and set operations process 64 of them per instruction.
- **Speed.** `x & (n-1)` instead of `x % n`, `popcount` in one instruction,
  `x & -x` to jump through a Fenwick tree.
- **Representing sets.** A subset of ≤ 64 items *is* an integer. That turns
  "try every subset" into `for (mask = 0; mask < 1<<n; mask++)`, and makes
  bitmask DP possible.
- **Data on the wire.** Network packets and file formats fix the byte order.
  Your CPU may disagree (endianness).

## 2. Mental model

**A row of light switches with numbered positions.** Position 0 is on the
right. An integer is just the pattern of on/off switches; arithmetic is one
way to read the pattern, and bit operations change switches directly.

```
  bit position:  7   6   5   4   3   2   1   0
               +---+---+---+---+---+---+---+---+
   0xB5   =    | 1 | 0 | 1 | 1 | 0 | 1 | 0 | 1 |
               +---+---+---+---+---+---+---+---+
  weight:      128  64  32  16   8   4   2   1      -> 128+32+16+4+1 = 181
               \______ B ______/ \______ 5 ______/   one hex digit = 4 bits
```

The four tools, each acting on a *mask* (a pattern that selects switches):

```
  OR  |  : turn selected switches ON          x | mask
  AND &  : keep only selected switches        x & mask       (test / extract)
  AND ~  : turn selected switches OFF         x & ~mask
  XOR ^  : flip selected switches             x ^ mask
  SHIFT  : slide the whole row left/right     x << n , x >> n
```

## 3. How it works

### 3.1 Binary, hex, two's complement

- Hex is a shorthand for binary: each hex digit is exactly one **nibble**.
  `0xB5` → `1011 0101`. This is why datasheets use hex: you can read the bits.
- **Two's complement** stores negatives so that the same adder works for
  signed and unsigned. The top bit has a *negative* weight:

```
  int8_t bit weights:  -128  64  32  16   8   4   2   1
  -5 = 1111 1011  =    -128 +64 +32 +16  +8      +2  +1  = -5

  To negate:  invert all bits, add 1         -x == ~x + 1
       5 = 0000 0101
      ~5 = 1111 1010
   ~5+1  = 1111 1011  = -5   ✓
```

- Range of N bits is `-2^(N-1) .. 2^(N-1)-1`: one more negative than
  positive. So `-INT_MIN` overflows (UB) and `abs(INT_MIN)` is UB.
- **Sign extension**: widening a signed value copies the top bit leftwards
  (`int8 -5 = FB` → `int16 FFFB`). Unsigned values are zero-extended.
  To sign-extend an N-bit field from hardware (12-bit ADC, 24-bit audio):
  `(int)(raw ^ signbit) - signbit`.

Program: `c/01_binary_hex_twos_complement.c`.

### 3.2 Shifts

```
  x << n  : multiply by 2^n       0000 0101 << 2 = 0001 0100   (5 -> 20)
  x >> n  : divide by 2^n

  unsigned 0xF0 >> 2  : 1111 0000 -> 0011 1100   zeros come in   (LOGICAL)
  signed   -16  >> 2  : 1111 0000 -> 1111 1100   sign comes in   (ARITHMETIC, = -4)
```

| Expression (32-bit `int`/`uint32_t`) | Status |
|---|---|
| `x << 32`, `x >> 32`, `x << -1` | **UB**: shift count must be `0 .. width-1`. On x86 the CPU masks the count, so `x << 32 == x` — a silent bug. |
| `1 << 31` | **UB** in C (overflows `int` into the sign bit). Write `1u << 31`. |
| `-1 << 1` | **UB** in C11 (left shift of negative). Shift unsigned values. |
| `uint8_t b = 0xFF; b << 24` | `b` is promoted to **int**: `0xFF << 24` overflows → **UB**. Write `(uint32_t)b << 24`. |
| `-16 >> 2` | Implementation-defined (gcc: arithmetic). Rounds toward −∞: `-7 >> 1 == -4`, but `-7 / 2 == -3`. |

Rule of thumb: **do bit manipulation on unsigned types**, of the width you
actually mean (`uint32_t`, not `int` or `long`).

Rotate: `(x << n) | (x >> (32 - n))` with `n` masked to `0..31` — gcc emits
one `ROL`/`ROR` instruction. Program: `c/02_shifts_and_masks.c`.

### 3.3 Masks: set, clear, toggle, test

```
  x           = 0000 1010
  set bit 0   : x |  (1u<<0)  = 0000 1011
  clear bit 3 : x & ~(1u<<3)  = 0000 0010
  toggle bit 7: x ^  (1u<<7)  = 1000 1010
  test bit 1  : (x >> 1) & 1  = 1
```

Multi-bit field `[h:l]` (the kernel's `GENMASK(h, l)`):

```
  GENMASK(7,4) = (~0u << 4) & (~0u >> (31-7))
               = 1111...1111 0000  &  0000...1111 1111
               = 0000...1111 0000 = 0xF0

  extract : (reg & mask) >> l
  insert  : (reg & ~mask) | ((val << l) & mask)
```

Building from `~0u` (all ones) instead of `(1u << (h+1)) - 1` avoids the
`1u << 32` UB when `h == 31`.

### 3.4 Register bitfields in drivers (read-modify-write)

A peripheral's registers are memory-mapped: a `volatile uint32_t` at a fixed
physical address (in Linux: `ioremap()` + `readl()`/`writel()`).

```
  mock UART CTRL register
   31            16 15     12 11     8 7      4  3   2   1   0
  +----------------+---------+--------+--------+---+---+---+---+
  |    reserved    | BAUDDIV | PARITY |  DLEN  |LBK|RXE|TXE|EN |
  +----------------+---------+--------+--------+---+---+---+---+
```

To change only `BAUDDIV`:

```
  1. READ      v = readl(CTRL)                    1001 0010 0011 0111   (0x9237)
  2. MODIFY    v &= ~BAUDDIV_MASK                 0000 0010 0011 0111   punch the hole
               v |= FIELD_PREP(BAUDDIV_MASK, 4)   0100 0010 0011 0111   drop in new value
  3. WRITE     writel(v, CTRL)                                          (0x4237)
```

Things that bite in real drivers:

- **Plain write instead of RMW** — `writel(CTRL_LBK, CTRL)` "sets loopback"
  and also clears EN, TXE, DLEN... the UART stops.
- **RMW is not atomic.** An IRQ handler doing its own RMW between your read
  and write gets its change lost. Protect with a spinlock, or use hardware
  SET/CLR registers (TI GPIO has `GPIO_SETDATAOUT` / `GPIO_CLEARDATAOUT`) —
  a single write that touches only the 1 bits.
- **W1C (write-1-to-clear) status registers** must *not* be RMW'd: writing
  back the value you read clears *every* pending flag, losing interrupts.
  Write only the bits you handled: `writel(STATUS_TXDONE, STATUS)`.
- **Reserved bits**: preserve or write 0 as the TRM says.
- **Don't use C bitfields (`unsigned en:1;`) for hardware.** Bit order,
  padding and access width are implementation-defined; the compiler may use
  byte accesses on a register that only accepts 32-bit ones. Use masks.
- `volatile` stops the compiler from caching or eliding the access. It does
  **not** order accesses against other CPUs/DMA — that is what barriers in
  `readl`/`writel` are for.

C version: `c/03_register_bitfields.c` (mock hardware model with W1C and
reserved bits). C++ version: `cpp/02_register_field_template.cpp` (each
field is a type bound to its register; wrong register or too-large constant
= compile error; zero overhead).

### 3.5 Popcount, powers of two, lowest set bit

The single most useful identity in this lesson:

```
  x & (x - 1)   deletes the LOWEST set bit

     x     = 0101 1000
     x - 1 = 0101 0111     borrowing flips the lowest 1 and the 0s below it
     AND   = 0101 0000

  x & -x        ISOLATES the lowest set bit      (-x == ~x + 1)

     x     = 0101 1000
    -x     = 1010 1000
     AND   = 0000 1000
```

From it:

| Want | Expression | Why |
|---|---|---|
| Is power of two? | `x && !(x & (x-1))` | power of two has exactly one bit; `x &&` excludes 0 |
| Popcount (Kernighan) | `while (x) { x &= x-1; c++; }` | one iteration per **set** bit |
| Lowest set bit value | `x & -x` | Fenwick tree step |
| Index of lowest bit | `__builtin_ctz(x)` | count trailing zeros; **UB for 0** |
| Index of highest bit | `31 - __builtin_clz(x)` | = floor(log2 x); **UB for 0** |
| Round up to power of 2 | smear: `x--; x \|= x>>1; ...>>16; x+1` | fills every bit below the top 1 |
| `x % 2^k` | `x & (2^k - 1)` | why ring buffers and hash tables use power-of-two sizes |
| Align up to `a` (power of 2) | `(x + a-1) & ~(a-1)` | the kernel's `ALIGN()` |

Kernighan trace on `0x58`:

```
  step 1: 0101 1000 -> 0101 0000
  step 2: 0101 0000 -> 0100 0000
  step 3: 0100 0000 -> 0000 0000      3 iterations = 3 set bits
```

Popcount methods (all checked against each other in `c/04_popcount_power_of_two.c`):
naive O(width), Kernighan O(set bits), byte table O(width/8), SWAR O(1)
(add pairs, then nibbles, then bytes, then multiply by `0x01010101` to sum
the bytes), and `__builtin_popcount` → one `POPCNT` (x86) / `CNT` (ARM NEON)
instruction.

### 3.6 XOR tricks and Gray code

XOR is addition without carry. Laws: `a^a = 0`, `a^0 = a`, commutative,
associative. Consequences:

- **Single number**: XOR every element; pairs cancel, the loner remains.
- **Two single numbers**: `s = p ^ q`; any set bit of `s` splits the array
  into two groups each containing one of them.
- **Missing number in 0..n**: XOR of indices ^ XOR of values.
- **Parity**: fold `x ^= x >> 16; ... x ^= x >> 1;` → bit 0. RAID-5's parity
  disk is the XOR of the data disks; lose one, XOR the rest to rebuild it.
- **XOR swap**: `a^=b; b^=a; a^=b;` — breaks if `a` and `b` are the same
  object (becomes 0). Use a temporary in real code; it is as fast.

**Gray code** `g = n ^ (n >> 1)`: consecutive values differ in one bit.

```
  n  binary  gray        3 -> 4 in binary flips 3 bits (011 -> 100):
  0  000     000         a reader sampling mid-change can see ANY value.
  1  001     001         In Gray it flips one bit (010 -> 110):
  2  010     011         the reader sees either old or new. Never garbage.
  3  011     010
  4  100     110         Used in: rotary/shaft encoders, async FIFO pointers
  5  101     111         crossing clock domains (FPGA, SoC interconnect),
  6  110     101         Karnaugh maps, genetic algorithms.
  7  111     100
```

Decode: binary bit i = XOR of all Gray bits at and above i (prefix XOR):
`g ^= g>>16; g ^= g>>8; ... g ^= g>>1;`.

Programs: `c/05_xor_tricks_gray_code.c`, `cpp/03_constexpr_bit_tricks.cpp`.

### 3.7 Bitsets

```
  bit i lives in word i/64, at position i%64       (i >> 6, i & 63)

   bits 0..63        bits 64..127      bits 128..191
  [  word 0   ]     [  word 1   ]     [  word 2   ]
```

- `std::bitset<N>`: size fixed at compile time; `set/reset/flip/test/count`,
  `& | ^ ~ << >>`, `to_string`, `to_ulong`. `test(i)` throws on out of range,
  `operator[]` does not check.
- `std::vector<bool>`: runtime size, packed, but a weird proxy-reference
  container (not a real `vector`; `&v[0]` is not `bool*`).
- Hand-written dynamic bitset on `std::vector<uint64_t>` (in `cpp/01_bitset.cpp`):
  `count()` = one popcount per word, `find_next()` skips zero words and uses
  `ctz` — that is how the kernel's `find_next_bit()` works.
- Sieve of Eratosthenes on a bitset: primes ≤ 10^6 in 122 KiB.

### 3.8 Endianness and byte swapping

```
  value 0x11223344 at address A:

  address:        A+0  A+1  A+2  A+3
  little endian:  44   33   22   11     x86, ARM Linux (TDA4VM A72, R5F)
  big endian:     11   22   33   44     network byte order, many file formats
```

- **Best practice: assemble from bytes with shifts.**
  `v = p[0]<<24 | p[1]<<16 | p[2]<<8 | p[3]` reads big endian on *any* host,
  at *any* alignment, with no `#ifdef`. gcc compiles it to a load + `REV`.
- Byte swap: mask and shift each byte to its mirror slot, or
  `__builtin_bswap16/32/64`, `htonl/ntohl`, kernel `be32_to_cpu()`,
  `cpu_to_le32()`, glibc `<endian.h>` `be32toh()`. C++23 has `std::byteswap`.
- **Never** `*(uint32_t *)(pkt + 1)`: unaligned (faults on some ARM cores,
  UB in C) and violates strict aliasing. Use `memcpy` or byte assembly.
- C++ trick (`cpp/04_endian_types.cpp`): a `Big<uint32_t>` wrapper holding
  bytes in network order with no implicit conversion — forgetting to convert
  becomes a compile error, like the kernel's `__be32` + `sparse`.

### 3.9 Subset enumeration with bitmasks

```
  items {A,B,C}  -> bit 0 = A, bit 1 = B, bit 2 = C

  mask  CBA   subset           union      a | b
   0    000   {}               intersect  a & b
   1    001   {A}              minus      a & ~b
   2    010   {B}              subset?    (a & b) == a
   3    011   {A,B}            size       popcount(a)
   4    100   {C}              add i      a | 1u<<i
   5    101   {A,C}            remove i   a & ~(1u<<i)
   6    110   {B,C}
   7    111   {A,B,C}
```

- **All subsets**: `for (mask = 0; mask < (1u << n); mask++)` — 2^n masks.
- **All submasks of m** (decreasing): `for (s = m; ; s = (s-1) & m) { ...; if (!s) break; }`.
  `s-1` clears the lowest 1 and sets everything below; `& m` keeps only
  bits of `m`. Summed over all `m`, this is 3^n iterations (each item is:
  not in m / in m not in s / in both).
- **All k-subsets in order** (Gosper's hack):
  `c = x & -x; r = x + c; x = (((r ^ x) >> 2) / c) | r;`
- **State compression**: N-Queens with `cols | diag1 | diag2` masks; TSP
  `dp[mask][last]` — 2^n·n² instead of n! (`cpp/05_bitmask_search.cpp`).
  Bitmask DP is developed fully in lesson 17.

## 4. Complexity table

| Operation | Time | Space | Why |
|---|---|---|---|
| set/clear/toggle/test one bit | O(1) | O(1) | one ALU instruction |
| extract/insert field (RMW) | O(1) | O(1) | AND, OR, shift; one bus read + one write for MMIO |
| popcount naive | O(w) | O(1) | look at each of w bits |
| popcount Kernighan | O(k) | O(1) | k = number of set bits |
| popcount table / SWAR / builtin | O(w/8) / O(log w) / O(1) | 256 B / O(1) / O(1) | byte lookups / parallel adds / hardware |
| is power of two, lowest bit | O(1) | O(1) | `x & (x-1)`, `x & -x` |
| ctz / clz | O(1) | O(1) | `TZCNT`/`LZCNT` (x86), `RBIT+CLZ`/`CLZ` (ARM) |
| round up to power of two | O(log w) | O(1) | 5 smear steps for 32 bits |
| byte swap | O(1) | O(1) | `BSWAP` / `REV` |
| bitset of n bits: count, `&`, `\|` | O(n/64) | n/8 bytes | one op per 64-bit word |
| bitset find_next | O(n/64) worst | — | skips zero words, ctz within word |
| enumerate all subsets | O(2^n) | O(1) | every mask once |
| enumerate submasks of all masks | O(3^n) | O(1) | 3 states per item |
| Gosper k-subsets | O(C(n,k)) | O(1) | O(1) per step |

## 5. C vs C++

| Need | C | C++ |
|---|---|---|
| One bit | `x \|= 1u << n` macros | same, or `std::bitset::set(n)` |
| Field masks | `#define GENMASK`, `FIELD_PREP/GET` macros | `constexpr` functions, `Field<Reg,Hi,Lo>` types with `static_assert` |
| popcount / ctz / clz | `__builtin_popcount`, `__builtin_ctz`, `__builtin_clz` (gcc/clang) | C++20 `<bit>`: `std::popcount`, `std::countr_zero`, `std::countl_zero`, `std::has_single_bit`, `std::bit_ceil`, `std::rotl` |
| Fixed bitset | `uint64_t words[N/64]` + macros | `std::bitset<N>` |
| Dynamic bitset | malloc'd `uint64_t[]` | `std::vector<bool>` or a class on `vector<uint64_t>` |
| Byte swap | `__builtin_bswap32`, `htonl`, `be32toh` | C++23 `std::byteswap`, C++20 `std::endian` |
| Type-pun bytes | `memcpy` | `memcpy` or C++20 `std::bit_cast` (never `reinterpret_cast` + deref) |

C has C23 `<stdbit.h>` (`stdc_count_ones`, `stdc_trailing_zeros`, ...) —
this repo uses C11, so builtins.

## 6. Where it is used in the real world

- **Linux kernel**: `include/linux/bits.h` (`BIT`, `GENMASK`),
  `linux/bitfield.h` (`FIELD_PREP`, `FIELD_GET`), `readl`/`writel` +
  `regmap_update_bits()` for RMW in drivers, `hweight32()` (popcount),
  `fls()`/`ffs()`/`__ffs()`, `roundup_pow_of_two()`, `ALIGN()`,
  `DECLARE_BITMAP`/`find_next_bit()` (CPU masks, IRQ allocation, PID map),
  `be32_to_cpu`, `get_unaligned_be32()`. Page flags are bits in
  `struct page->flags`.
- **TDA4VM / BeagleBone AI-64**: every TRM register table (CTRL_MMR, pinmux
  `PADCONFIG` fields, GPIO `SET/CLR` registers), device-tree `interrupts`
  flags, the `0x...` values in U-Boot `mw`/`md` commands.
- **GStreamer / video**: `GstBufferFlags`, `GstElementFlags` and caps flags
  are bitmasks; pixel formats pack fields (RGB565: `r = (p >> 11) & 0x1F`);
  H.264 bitstream parsing reads Exp-Golomb codes bit by bit (`GstBitReader`);
  NAL headers (`nal_unit_type = byte & 0x1F`).
- **Networking**: IP/TCP headers are big-endian bit fields (`ihl = b & 0x0F`,
  TCP flags SYN/ACK/FIN bits), subnet masks (`ip & mask == net`), CRC.
- **File systems & databases**: block allocation bitmaps (ext4), Bloom
  filters, bitmap indexes, roaring bitmaps.
- **Unix permissions**: `mode & S_IRUSR`, `chmod 755` = `111 101 101`.
- **Games/engines**: chess bitboards (64 squares = one `uint64_t`), collision
  layers, ECS component masks.
- **Compilers/CPUs**: strength reduction (`x*8` → `x<<3`), register
  allocation sets, instruction encoding.

## 7. Common mistakes and pitfalls

1. `1 << 31` or `1 << n` with `n >= 31` on `int` → UB. Use `1u`, `1ull`, `UINT64_C(1)`.
2. Shift count ≥ width (`x << 32`) → UB; on x86 silently `x`.
3. Integer promotion: `uint8_t`/`uint16_t` become `int` before shifting;
   `~(uint8_t)0` is `-1` (int), not `0xFF`. Cast back or mask.
4. Operator precedence: `x & mask == 0` parses as `x & (mask == 0)`. Always
   parenthesise: `(x & mask) == 0`. Same for `<<` vs `+`: `1 << n + 1` is `1 << (n+1)`.
5. `is_pow2` without the `x != 0` guard says 0 is a power of two.
6. `__builtin_ctz(0)` / `__builtin_clz(0)` are UB.
7. Right-shifting negative signed values: implementation-defined, and rounds
   toward −∞ — not the same as division.
8. Writing a register instead of read-modify-writing it; RMW on W1C
   registers; RMW without locking against IRQ handlers.
9. C bitfields for hardware layouts or wire formats.
10. Casting a byte pointer to `uint32_t *` for unaligned / aliased access.
11. Forgetting byte order when sending structs over the network or writing them to files.
12. XOR swap of the same object zeroes it.
13. Submask loop `for (s = m; s; s = (s-1) & m)` silently skips the empty set;
    `for (s = m; s >= 0; ...)` with unsigned `s` never terminates.
14. `std::bitset` string constructor: leftmost char is the **highest** bit.

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_binary_hex_twos_complement.c](c/01_binary_hex_twos_complement.c) | binary/hex conversion, `~x+1`, INT_MIN asymmetry, sign extension of N-bit fields | `make run P=dsa/18-bit-manipulation/c/01_binary_hex_twos_complement.c` |
| [c/02_shifts_and_masks.c](c/02_shifts_and_masks.c) | logical vs arithmetic shift, UB cases, set/clear/toggle/test, GENMASK, field extract/insert, rotate | `make run P=dsa/18-bit-manipulation/c/02_shifts_and_masks.c` |
| [c/03_register_bitfields.c](c/03_register_bitfields.c) | mock UART peripheral: FIELD_PREP/GET, read-modify-write, plain-write bug, reserved bits, W1C status | `make run P=dsa/18-bit-manipulation/c/03_register_bitfields.c` |
| [c/04_popcount_power_of_two.c](c/04_popcount_power_of_two.c) | naive/Kernighan/SWAR/table/builtin popcount, is_pow2, `x&-x`, ctz/clz, round up to pow2 | `make run P=dsa/18-bit-manipulation/c/04_popcount_power_of_two.c` |
| [c/05_xor_tricks_gray_code.c](c/05_xor_tricks_gray_code.c) | XOR swap, single/two-single/missing number, parity, Gray encode/decode | `make run P=dsa/18-bit-manipulation/c/05_xor_tricks_gray_code.c` |
| [c/06_endianness_byteswap.c](c/06_endianness_byteswap.c) | detect endianness, manual vs builtin bswap, portable big-endian packet parsing | `make run P=dsa/18-bit-manipulation/c/06_endianness_byteswap.c` |
| [c/07_subset_enumeration.c](c/07_subset_enumeration.c) | all subsets, set algebra, subset sum, submask enumeration (3^n), Gosper's hack | `make run P=dsa/18-bit-manipulation/c/07_subset_enumeration.c` |
| [cpp/01_bitset.cpp](cpp/01_bitset.cpp) | `std::bitset`, hand-written dynamic bitset with `find_next`, bitset sieve | `make run P=dsa/18-bit-manipulation/cpp/01_bitset.cpp` |
| [cpp/02_register_field_template.cpp](cpp/02_register_field_template.cpp) | type-safe `Field<Reg,Hi,Lo>` / `Reg<Tag>` with compile-time checks and multi-field RMW | `make run P=dsa/18-bit-manipulation/cpp/02_register_field_template.cpp` |
| [cpp/03_constexpr_bit_tricks.cpp](cpp/03_constexpr_bit_tricks.cpp) | generic constexpr popcount, bit_ceil, rotl, reverse_bits, Gray, align_up — proven by `static_assert` | `make run P=dsa/18-bit-manipulation/cpp/03_constexpr_bit_tricks.cpp` |
| [cpp/04_endian_types.cpp](cpp/04_endian_types.cpp) | generic `byteswap<T>`, `load_be/le`, `Big<T>` wire type, UDP header parse/build | `make run P=dsa/18-bit-manipulation/cpp/04_endian_types.cpp` |
| [cpp/05_bitmask_search.cpp](cpp/05_bitmask_search.cpp) | N-Queens with bitmasks, TSP bitmask DP, both vs brute force | `make run P=dsa/18-bit-manipulation/cpp/05_bitmask_search.cpp` |

## 9. Exercises

1. **(easy)** Write `int count_bits_to_flip(uint32_t a, uint32_t b)` (Hamming
   distance). *Hint: popcount of `a ^ b`.*
2. **(easy)** Reverse the bits of a byte with a 16-entry nibble table.
   *Hint: `rev(b) = tab[b & 0xF] << 4 | tab[b >> 4]`.*
3. **(easy)** Decode RGB565 into 8-bit R, G, B, expanding 5→8 bits correctly.
   *Hint: `r8 = (r5 << 3) | (r5 >> 2)` replicates the top bits.*
4. **(medium)** Given 0..n with every number appearing **three** times except
   one, find it in O(1) space. *Hint: count each bit position mod 3.*
5. **(medium)** Add a `GPIO_SET`/`GPIO_CLR` register pair to the mock in
   `c/03_register_bitfields.c` and show that two "threads" (interleaved
   by hand) lose an update with RMW but not with SET/CLR.
6. **(medium)** Implement a bit reader (`get_bits(n)`, `get_ue()` for
   Exp-Golomb) over a byte buffer, and decode an H.264 SPS `profile_idc` and
   `level_idc`. *Hint: keep a bit position; `ue = 2^zeros - 1 + next(zeros)`.*
7. **(medium)** Find the next larger number with the same popcount *without*
   division (alternative Gosper using ctz).
8. **(hard)** Maximum XOR of any two numbers in an array in O(n·32).
   *Hint: binary trie (lesson 11), or greedy prefix set bit by bit.*
9. **(hard)** Count Hamiltonian paths in a 16-node graph with bitmask DP and
   compare against `next_permutation` for n ≤ 9.
10. **(hard)** Implement a 4096-bit allocator bitmap with `alloc_contig(k)`
    that finds k consecutive free bits using word-level tricks.

## 10. Remember forever

```
x & (x-1)   drops lowest 1      -> popcount loop, is_pow2 = x && !(x&(x-1))
x & -x      keeps lowest 1      -> Fenwick step;  -x == ~x + 1
set |= 1u<<n   clear &= ~(1u<<n)   toggle ^= 1u<<n   test (x>>n)&1
field: get (r & m) >> l ; put (r & ~m) | (v << l & m) ; GENMASK from ~0u
MMIO: read-modify-write; never RMW a W1C register; plain write = clobber
Shift unsigned only; count in 0..w-1; cast before shifting small types
Gray = n ^ (n>>1) : neighbours differ in one bit
Endian: build values from bytes with shifts; never cast unaligned pointers
Subsets: for m < 1<<n ; submasks: s = (s-1) & m  (3^n total)
```

Review with the spaced-repetition schedule in
[LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

## 11. Self-test questions

1. Why does `x & (x - 1)` remove exactly the lowest set bit? Draw the borrow.
2. What is `-5` as an 8-bit pattern, and why is the top bit's weight −128?
3. Give four shift expressions that are undefined behaviour in C.
4. Why is `-7 >> 1` not equal to `-7 / 2`?
5. Write `GENMASK(h, l)` so that `GENMASK(31, 0)` does not invoke UB.
6. Walk through a read-modify-write of a 4-bit field. Where can an
   interrupt break it, and what are two fixes?
7. What is a W1C register, and what goes wrong if you RMW it?
8. Why should you not describe a hardware register with a C bitfield struct?
9. Explain three different ways to compute popcount and their complexities.
10. Why do ring buffers and hash tables like power-of-two sizes?
11. What property does Gray code have, and where does hardware rely on it?
12. Show the bytes of `0x11223344` in little- and big-endian memory. How do
    you read a big-endian `uint32_t` from a packet portably?
13. How many iterations does enumerating all submasks of all n-bit masks take, and why?
14. How does N-Queens become fast with three bitmasks?
