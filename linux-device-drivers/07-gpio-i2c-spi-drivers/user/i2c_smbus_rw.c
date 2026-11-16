/*
 * i2c_smbus_rw.c — talk to an I2C device from user space through i2c-dev:
 *                  /dev/i2c-N + I2C_SLAVE + raw I2C_SMBUS ioctls
 *
 * Concept : i2c-dev turns every adapter into /dev/i2c-N. You open it, pick
 *           the 7-bit target address with ioctl(I2C_SLAVE) and then either
 *             - ioctl(I2C_SMBUS, {read_write, command=reg, size, data})
 *               (what libi2c's i2c_smbus_read_byte_data() and i2cget do), or
 *             - ioctl(I2C_RDWR, i2c_msg[]) for raw combined transfers
 *               (only if the adapter has I2C_FUNC_I2C; i2c-stub has not).
 *           I2C_SLAVE fails with EBUSY if a kernel driver is bound to that
 *           address: user space and a driver must not both drive one chip.
 *           (i2cget -f uses I2C_SLAVE_FORCE to override that. Don't.)
 *           Safety: with no arguments it ONLY uses an i2c-stub adapter
 *           (writing to random addresses on a PC's SMBus can corrupt DIMM
 *           SPD EEPROMs). On real hardware give BUS ADDR; it only reads
 *           unless you add -w.
 * Build   : make drivers  (-> build/linux-device-drivers/07-gpio-i2c-spi-drivers/user/i2c_smbus_rw)
 * Run     : VM:    sudo modprobe i2c-stub chip_addr=0x50 ; sudo ./i2c_smbus_rw
 *           Board: sudo ./i2c_smbus_rw 2 0x50 0x10        read reg 0x10 on /dev/i2c-2
 *                  sudo ./i2c_smbus_rw 2 0x50 0x10 -w     write/verify/restore
 *           Exit 0 = OK, 1 = a CHECK failed, 2 = no suitable adapter/device.
 *           Not run by `make test`.
 * Read    : ../README.md  (section 3.3 "I2C", user-space part)
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%s\n", #cond,   \
                    __FILE__, __LINE__, strerror(errno));                    \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/* The one SMBus entry point: everything below is built on it. */
static int smbus(int fd, char rw, unsigned char cmd, int size,
                 union i2c_smbus_data *data)
{
    struct i2c_smbus_ioctl_data args;

    args.read_write = rw;   /* I2C_SMBUS_READ or I2C_SMBUS_WRITE */
    args.command = cmd;     /* the "register" byte sent first */
    args.size = size;       /* I2C_SMBUS_BYTE_DATA, _WORD_DATA, ... */
    args.data = data;
    return ioctl(fd, I2C_SMBUS, &args);
}

/*  S | addr W | A | reg | A | Sr | addr R | A | data | NA | P  */
static int read_byte_data(int fd, unsigned char reg)
{
    union i2c_smbus_data d;

    if (smbus(fd, I2C_SMBUS_READ, reg, I2C_SMBUS_BYTE_DATA, &d) < 0)
        return -errno;
    return d.byte;
}

/*  S | addr W | A | reg | A | data | A | P  */
static int write_byte_data(int fd, unsigned char reg, unsigned char val)
{
    union i2c_smbus_data d;

    d.byte = val;
    return smbus(fd, I2C_SMBUS_WRITE, reg, I2C_SMBUS_BYTE_DATA, &d) < 0 ? -errno : 0;
}

static int read_word_data(int fd, unsigned char reg)
{
    union i2c_smbus_data d;

    if (smbus(fd, I2C_SMBUS_READ, reg, I2C_SMBUS_WORD_DATA, &d) < 0)
        return -errno;
    return d.word; /* SMBus words are little endian on the wire */
}

static int write_word_data(int fd, unsigned char reg, unsigned short val)
{
    union i2c_smbus_data d;

    d.word = val;
    return smbus(fd, I2C_SMBUS_WRITE, reg, I2C_SMBUS_WORD_DATA, &d) < 0 ? -errno : 0;
}

/* I2C-block read: d.block[0] = length in/out, d.block[1..] = data */
static int read_i2c_block(int fd, unsigned char reg, unsigned char *buf, int len)
{
    union i2c_smbus_data d;

    if (len > I2C_SMBUS_BLOCK_MAX)
        len = I2C_SMBUS_BLOCK_MAX;
    d.block[0] = (unsigned char)len;
    if (smbus(fd, I2C_SMBUS_READ, reg, I2C_SMBUS_I2C_BLOCK_DATA, &d) < 0)
        return -errno;
    memcpy(buf, &d.block[1], d.block[0]);
    return d.block[0];
}

/* Find the i2c-stub adapter by name in sysfs; -1 if not loaded. */
static int find_stub_bus(void)
{
    int bus;

    for (bus = 0; bus < 256; bus++) {
        char path[64], name[128] = "";
        FILE *f;

        snprintf(path, sizeof(path), "/sys/class/i2c-dev/i2c-%d/name", bus);
        f = fopen(path, "r");
        if (!f)
            continue;
        if (!fgets(name, sizeof(name), f))
            name[0] = '\0';
        fclose(f);
        if (strncmp(name, "SMBus stub driver", 17) == 0)
            return bus;
    }
    return -1;
}

static int open_dev(int bus, int addr)
{
    char path[32];
    unsigned long funcs = 0;
    int fd;

    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "%s: %s%s\n", path, strerror(errno),
                errno == ENOENT ? " (modprobe i2c-dev)" : " (try sudo)");
        exit(2);
    }
    CHECK(ioctl(fd, I2C_FUNCS, &funcs) == 0);
    printf("%s: functionality 0x%08lx:%s%s%s%s\n", path, funcs,
           (funcs & I2C_FUNC_I2C) ? " I2C(raw)" : "",
           (funcs & I2C_FUNC_SMBUS_BYTE_DATA) ? " byte-data" : "",
           (funcs & I2C_FUNC_SMBUS_WORD_DATA) ? " word-data" : "",
           (funcs & I2C_FUNC_SMBUS_I2C_BLOCK) ? " i2c-block" : "");
    if (!(funcs & I2C_FUNC_SMBUS_BYTE_DATA)) {
        fprintf(stderr, "adapter cannot do SMBus byte data\n");
        exit(2);
    }
    /* I2C_SLAVE takes the address as the argument VALUE, not a pointer. */
    if (ioctl(fd, I2C_SLAVE, (unsigned long)addr) < 0) {
        fprintf(stderr, "I2C_SLAVE 0x%02x: %s%s\n", addr, strerror(errno),
                errno == EBUSY ? " (a kernel driver is bound: delete_device or unbind it)" : "");
        exit(2);
    }
    return fd;
}

/* VM self-test on i2c-stub: it behaves like a 256-register chip. */
static int stub_test(int bus, int addr)
{
    unsigned char blk[8];
    int fd = open_dev(bus, addr), v, i;

    v = read_byte_data(fd, 0x00);
    if (v == -ENXIO || v == -EREMOTEIO) {
        fprintf(stderr, "no chip at 0x%02x on the stub: modprobe i2c-stub chip_addr=0x%02x\n",
                addr, addr);
        return 2;
    }
    CHECK(v >= 0);

    CHECK(write_byte_data(fd, 0x10, 0xa5) == 0);
    CHECK(read_byte_data(fd, 0x10) == 0xa5);
    printf("PASS byte data: wrote 0xa5 to reg 0x10 and read it back\n");

    CHECK(write_word_data(fd, 0x20, 0x1234) == 0);
    CHECK(read_word_data(fd, 0x20) == 0x1234);
    /* little endian: low byte lands in reg 0x20, high byte in 0x21 */
    CHECK(read_byte_data(fd, 0x20) == 0x34 && read_byte_data(fd, 0x21) == 0x12);
    printf("PASS word data: 0x1234 -> reg 0x20 = 0x34, reg 0x21 = 0x12 (little endian)\n");

    for (i = 0; i < 8; i++)
        CHECK(write_byte_data(fd, (unsigned char)(0x30 + i), (unsigned char)(i * 3)) == 0);
    CHECK(read_i2c_block(fd, 0x30, blk, 8) == 8);
    for (i = 0; i < 8; i++)
        CHECK(blk[i] == i * 3);
    printf("PASS i2c block: 8 registers 0x30..0x37 read in one transfer\n");
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    int bus, addr, reg = 0, do_write = 0, fd, v;

    if (argc == 1) {
        bus = find_stub_bus();
        if (bus < 0) {
            fprintf(stderr,
                    "no i2c-stub adapter. In a VM: sudo modprobe i2c-dev; "
                    "sudo modprobe i2c-stub chip_addr=0x50\n"
                    "On real hardware: %s BUS ADDR [REG] [-w]\n", argv[0]);
            return 2;
        }
        printf("using i2c-stub on bus %d, address 0x50\n", bus);
        return stub_test(bus, 0x50);
    }
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "usage: %s [BUS ADDR [REG] [-w]]\n", argv[0]);
        return 2;
    }
    bus = (int)strtol(argv[1], NULL, 0);
    addr = (int)strtol(argv[2], NULL, 0);
    if (argc >= 4 && strcmp(argv[3], "-w") != 0)
        reg = (int)strtol(argv[3], NULL, 0);
    do_write = strcmp(argv[argc - 1], "-w") == 0;
    if (addr < 0x03 || addr > 0x77 || reg < 0 || reg > 0xff) {
        fprintf(stderr, "address must be 0x03..0x77, register 0..0xff\n");
        return 2;
    }

    fd = open_dev(bus, addr);
    v = read_byte_data(fd, (unsigned char)reg);
    if (v < 0) {
        fprintf(stderr, "read reg 0x%02x at 0x%02x: %s (nothing ACKs?)\n",
                reg, addr, strerror(-v));
        return 2;
    }
    printf("bus %d addr 0x%02x reg 0x%02x = 0x%02x\n", bus, addr, reg, v);
    if (do_write) {
        unsigned char test = (unsigned char)(v ^ 0xff);
        int back; /* int, not u8: a negative errno must stay negative */

        CHECK(write_byte_data(fd, (unsigned char)reg, test) == 0);
        usleep(10000); /* EEPROM write cycle (tWR ~5 ms) */
        back = read_byte_data(fd, (unsigned char)reg);
        CHECK(write_byte_data(fd, (unsigned char)reg, (unsigned char)v) == 0);
        usleep(10000);
        CHECK(back == test);
        CHECK(read_byte_data(fd, (unsigned char)reg) == v);
        printf("PASS write 0x%02x, read back 0x%02x, restored 0x%02x\n",
               test, back, v);
    }
    close(fd);
    return 0;
}
