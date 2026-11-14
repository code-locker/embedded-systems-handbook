/*
 * 03_register_bitfields.c — driver-style register access on a MOCK peripheral:
 *                           field macros, read-modify-write, W1C status bits
 *
 * Concept : A memory-mapped peripheral is a struct of 32-bit registers at a
 *           fixed address. Each register packs several fields. To change one
 *           field you must READ the register, MODIFY only that field's bits,
 *           and WRITE it back (RMW) — otherwise you clobber the neighbours.
 *           Real drivers use `volatile` accessors (readl/writel) and
 *           FIELD_PREP/FIELD_GET, never C bitfields, because C bitfield layout
 *           is implementation-defined.
 *           Here the "hardware" is a plain struct in RAM plus a function that
 *           simulates what the silicon would do, so the program runs anywhere.
 * Build   : make run P=dsa/18-bit-manipulation/c/03_register_bitfields.c
 * Read    : ../README.md  (section "3.4 Register bitfields in drivers")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Mock UART-like peripheral. Register map (offsets in bytes):
 *
 *   0x00 CTRL    31            16 15     12 11  8 7  4  3   2   1   0
 *                +---------------+---------+-----+----+---+---+---+---+
 *                |   reserved    | BAUDDIV |PARITY|DLEN|LBK|RXE|TXE|EN |
 *                +---------------+---------+-----+----+---+---+---+---+
 *                 BAUDDIV = bits 15:12 (divider), PARITY = 11:8,
 *                 DLEN    = bits  7:4  (data length - 5),
 *                 LBK bit 3 (loopback), RXE bit 2, TXE bit 1, EN bit 0
 *
 *   0x04 STATUS  bit 0 TXDONE (W1C), bit 1 RXREADY (W1C), bit 8 BUSY (RO)
 *                W1C = "write 1 to clear": writing 1 clears the bit,
 *                writing 0 leaves it alone. So you must NOT RMW a W1C
 *                register (you would clear every pending flag you read!).
 *
 *   0x08 DATA    bits 7:0
 * ------------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t ctrl;    /* 0x00 */
    volatile uint32_t status;  /* 0x04 */
    volatile uint32_t data;    /* 0x08 */
} mock_uart_regs;

/* Offsets must match the datasheet; catch a padding mistake at compile time. */
_Static_assert(sizeof(mock_uart_regs) == 12, "register block must be 3 x 32 bit");

/* ---- generic field helpers (same idea as linux/bitfield.h) ---------------- */
#define BIT(n)          (1u << (n))
#define GENMASK(h, l)   ((~0u << (l)) & (~0u >> (31 - (h))))
/* Lowest set bit position of the mask = how far to shift. __builtin_ctz is
 * evaluated at compile time for constant masks, so this costs nothing. */
#define FIELD_SHIFT(m)      ((unsigned)__builtin_ctz(m))
#define FIELD_PREP(m, val)  (((uint32_t)(val) << FIELD_SHIFT(m)) & (m))
#define FIELD_GET(m, reg)   (((reg) & (m)) >> FIELD_SHIFT(m))

#define CTRL_EN        BIT(0)
#define CTRL_TXE       BIT(1)
#define CTRL_RXE       BIT(2)
#define CTRL_LBK       BIT(3)
#define CTRL_DLEN      GENMASK(7, 4)
#define CTRL_PARITY    GENMASK(11, 8)
#define CTRL_BAUDDIV   GENMASK(15, 12)
#define CTRL_RESERVED  GENMASK(31, 16)

#define STATUS_TXDONE  BIT(0)
#define STATUS_RXREADY BIT(1)
#define STATUS_BUSY    BIT(8)

/* ---- accessors: the ONLY place that touches the registers ----------------
 * In the kernel these are readl()/writel(), which are volatile accesses plus
 * memory barriers. Keeping every access in one function also lets us count
 * bus transactions — on real hardware each one costs ~100 ns over the bus. */
static unsigned g_reads, g_writes;

static uint32_t reg_read(volatile uint32_t *r)
{
    g_reads++;
    return *r;
}

/* Simulated silicon: what the hardware does when the CPU writes. */
static void reg_write(mock_uart_regs *dev, volatile uint32_t *r, uint32_t v)
{
    g_writes++;
    if (r == &dev->status) {
        /* W1C: clear only bits written as 1; BUSY is read-only. */
        dev->status &= ~(v & (STATUS_TXDONE | STATUS_RXREADY));
    } else if (r == &dev->ctrl) {
        dev->ctrl = v & ~CTRL_RESERVED;      /* reserved bits read back as 0 */
    } else if (r == &dev->data) {
        dev->data = v & 0xFFu;
        /* writing DATA while TX enabled "sends" the byte: in loopback mode it
         * comes straight back as received data. */
        if ((dev->ctrl & (CTRL_EN | CTRL_TXE)) == (CTRL_EN | CTRL_TXE)) {
            dev->status |= STATUS_TXDONE;
            if ((dev->ctrl & CTRL_LBK) && (dev->ctrl & CTRL_RXE))
                dev->status |= STATUS_RXREADY;
        }
    }
}

/* Read-modify-write of one field. O(1), 1 read + 1 write.
 *
 *   old       = 1010 0111 0101 0011
 *   mask      = 0000 0000 1111 0000     (DLEN)
 *   old&~mask = 1010 0111 0000 0011     <- hole punched for the field
 *   | new<<4  = 1010 0111 0011 0011     <- new value dropped into the hole
 *
 * NOT atomic: if an interrupt handler RMWs the same register between our
 * read and write, its change is lost. Drivers protect RMW with a spinlock,
 * or the hardware offers separate SET/CLR registers (e.g. GPIO_SETDATAOUT on
 * TI SoCs) which make single writes atomic. */
static void reg_update_field(mock_uart_regs *dev, volatile uint32_t *r,
                             uint32_t mask, uint32_t val)
{
    uint32_t v = reg_read(r);
    v &= ~mask;
    v |= FIELD_PREP(mask, val);
    reg_write(dev, r, v);
}

static void uart_configure(mock_uart_regs *dev, unsigned data_bits,
                           unsigned parity, unsigned baud_div)
{
    /* Build the whole value in a CPU register and write ONCE when we own the
     * whole register (init time): cheaper than three separate RMWs. */
    uint32_t v = FIELD_PREP(CTRL_DLEN, data_bits - 5u) |
                 FIELD_PREP(CTRL_PARITY, parity) |
                 FIELD_PREP(CTRL_BAUDDIV, baud_div) |
                 CTRL_EN | CTRL_TXE | CTRL_RXE;
    reg_write(dev, &dev->ctrl, v);
}

int main(void)
{
    /* Power-on reset values. Stack allocated: no ownership questions. */
    mock_uart_regs dev = { .ctrl = 0, .status = STATUS_BUSY, .data = 0 };

    puts("== 1. FIELD_PREP / FIELD_GET ==");
    assert(FIELD_SHIFT(CTRL_DLEN) == 4);
    assert(FIELD_PREP(CTRL_DLEN, 3) == 0x30u);
    assert(FIELD_GET(CTRL_PARITY, 0x0A50u) == 0xAu);
    /* Overflowing value is masked off rather than corrupting the neighbour: */
    assert(FIELD_PREP(CTRL_DLEN, 0x1F) == 0xF0u);
    puts("FIELD_PREP(DLEN,3)=0x30  FIELD_GET(PARITY,0xA50)=0xA");

    puts("\n== 2. Configure: 8 data bits, parity 2, divider 9, one write ==");
    uart_configure(&dev, 8, 2, 9);
    uint32_t ctrl = reg_read(&dev.ctrl);
    printf("CTRL = 0x%08X\n", (unsigned)ctrl);
    assert(FIELD_GET(CTRL_DLEN, ctrl) == 3);
    assert(FIELD_GET(CTRL_PARITY, ctrl) == 2);
    assert(FIELD_GET(CTRL_BAUDDIV, ctrl) == 9);
    assert(ctrl & CTRL_EN);
    assert(ctrl == 0x9237u);

    puts("\n== 3. RMW one field: change only the baud divider ==");
    reg_update_field(&dev, &dev.ctrl, CTRL_BAUDDIV, 4);
    ctrl = reg_read(&dev.ctrl);
    printf("CTRL = 0x%08X (divider now 4, everything else untouched)\n", (unsigned)ctrl);
    assert(ctrl == 0x4237u);

    puts("\n== 4. The classic bug: plain write instead of RMW ==");
    mock_uart_regs bad = dev;
    /* "just set loopback" — but this overwrites EN, TXE, DLEN... with 0 */
    reg_write(&bad, &bad.ctrl, CTRL_LBK);
    printf("plain write CTRL_LBK -> 0x%08X  (UART disabled by accident!)\n", (unsigned)bad.ctrl);
    assert((bad.ctrl & CTRL_EN) == 0);
    /* correct: RMW set of a single bit */
    reg_write(&dev, &dev.ctrl, reg_read(&dev.ctrl) | CTRL_LBK);
    printf("RMW   |= CTRL_LBK -> 0x%08X  (still enabled)\n", (unsigned)dev.ctrl);
    assert(dev.ctrl == 0x423Fu);

    puts("\n== 5. Reserved bits: write as read, expect zeros ==");
    reg_write(&dev, &dev.ctrl, reg_read(&dev.ctrl) | 0xFFFF0000u);
    assert((dev.ctrl & CTRL_RESERVED) == 0);
    puts("reserved bits [31:16] ignored by the hardware model");

    puts("\n== 6. W1C status: why you must NOT read-modify-write it ==");
    reg_write(&dev, &dev.data, 'A');          /* send a byte in loopback */
    uint32_t st = reg_read(&dev.status);
    printf("STATUS after TX = 0x%03X (TXDONE|RXREADY|BUSY)\n", (unsigned)st);
    assert(st == (STATUS_TXDONE | STATUS_RXREADY | STATUS_BUSY));
    /* We have handled TXDONE only. Correct ack: write ONLY that bit. */
    reg_write(&dev, &dev.status, STATUS_TXDONE);
    assert(dev.status == (STATUS_RXREADY | STATUS_BUSY));
    puts("ack TXDONE by writing 1<<0 -> RXREADY still pending (good)");
    /* Wrong ack: RMW writes back every 1 it read, clearing RXREADY too, so
     * the received byte's interrupt is silently lost. */
    mock_uart_regs bad2 = dev;
    reg_write(&bad2, &bad2.status, bad2.status | STATUS_TXDONE);
    assert((bad2.status & STATUS_RXREADY) == 0);
    puts("RMW on W1C register cleared RXREADY too -> lost event (bug shown)");

    printf("\nbus traffic: %u reads, %u writes\n", g_reads, g_writes);
    puts("all checks passed");
    return 0;
}
