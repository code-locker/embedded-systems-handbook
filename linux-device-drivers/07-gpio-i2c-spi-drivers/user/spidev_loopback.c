/*
 * spidev_loopback.c — full-duplex SPI loopback self-check through spidev:
 *                     /dev/spidevB.C + SPI_IOC_MESSAGE
 *
 * Concept : spidev exposes one SPI device (bus B, chip select C) to user
 *           space. Setup ioctls set mode (CPOL/CPHA), word size and clock;
 *           SPI_IOC_MESSAGE(n) runs n spi_ioc_transfer segments as ONE
 *           message with CS held low across them (unless cs_change=1).
 *           SPI is full duplex: every byte clocked out on MOSI clocks one
 *           byte in on MISO. With a jumper from MOSI to MISO, rx == tx.
 *
 *              master                       jumper
 *              MOSI ──────────────────────────┐
 *              MISO <─────────────────────────┘     rx[i] == tx[i]
 *              SCLK ──> (nothing)    CS ──> (nothing)
 *
 *           Board: SPI7 on P9.30 (MOSI) / P9.29 (MISO), see
 *           ../kernel/dts/esh-spi7-device.dtso and README section 3.5 for
 *           binding the device to spidev.
 * Build   : make drivers  (-> build/linux-device-drivers/07-gpio-i2c-spi-drivers/user/spidev_loopback)
 * Run     : sudo ./spidev_loopback                    first /dev/spidev* found
 *           sudo ./spidev_loopback /dev/spidev7.0 500000
 *           Exit 0 = rx == tx, 1 = mismatch / CHECK failed, 2 = no spidev device.
 *           Not run by `make test` (needs hardware).
 * Read    : ../README.md  (section 3.5 "SPI", user-space part)
 */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
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

#define LEN1 32
#define LEN2 7 /* odd size on purpose: no alignment assumptions */

static int find_spidev(char *out, size_t outlen)
{
    DIR *d = opendir("/dev");
    struct dirent *e;
    int found = 0;

    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "spidev", 6) == 0) {
            snprintf(out, outlen, "/dev/%s", e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int all_same(const uint8_t *b, size_t n, uint8_t v)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (b[i] != v)
            return 0;
    return 1;
}

int main(int argc, char **argv)
{
    char path[300];
    uint32_t mode = SPI_MODE_0, speed = 1000000, rd32 = 0;
    uint8_t bits = 8, rd8 = 0;
    uint8_t tx1[LEN1], rx1[LEN1], tx2[LEN2], rx2[LEN2];
    struct spi_ioc_transfer xfer[2];
    int fd, i, ret;

    if (argc > 1)
        snprintf(path, sizeof(path), "%s", argv[1]);
    else if (!find_spidev(path, sizeof(path))) {
        fprintf(stderr,
                "no /dev/spidev* device. On the board: load an SPI overlay and bind\n"
                "the device to spidev (README section 3.5), sudo modprobe spidev.\n"
                "There is no SPI simulator for a VM; this test needs hardware.\n");
        return 2;
    }
    if (argc > 2)
        speed = (uint32_t)strtoul(argv[2], NULL, 0);
    if (speed < 1000 || speed > 50000000) {
        fprintf(stderr, "speed %u Hz out of range 1000..50000000\n", speed);
        return 2;
    }

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "%s: %s (try sudo)\n", path, strerror(errno));
        return 2;
    }

    /* Write the setting, then read it back: the controller may refuse. */
    CHECK(ioctl(fd, SPI_IOC_WR_MODE32, &mode) == 0);
    CHECK(ioctl(fd, SPI_IOC_RD_MODE32, &rd32) == 0);
    CHECK((rd32 & (SPI_CPOL | SPI_CPHA)) == SPI_MODE_0);
    CHECK(ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) == 0);
    CHECK(ioctl(fd, SPI_IOC_RD_BITS_PER_WORD, &rd8) == 0);
    CHECK(rd8 == 8);
    CHECK(ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) == 0);
    CHECK(ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &rd32) == 0);
    printf("%s: mode %u, %u bits/word, max %u Hz\n", path,
           (unsigned int)(mode & (SPI_CPOL | SPI_CPHA)), rd8, rd32);

    for (i = 0; i < LEN1; i++)
        tx1[i] = (uint8_t)(0x5a ^ (i * 37)); /* never all-0 or all-1 */
    for (i = 0; i < LEN2; i++)
        tx2[i] = (uint8_t)(0xc3 + i);
    memset(rx1, 0, sizeof(rx1));
    memset(rx2, 0, sizeof(rx2));

    /*
     * Two segments, one message: CS stays asserted across both, like a
     * "command, then data" transaction to a real chip. Pointers travel as
     * __u64 so 32-bit user space works with a 64-bit kernel.
     */
    memset(xfer, 0, sizeof(xfer)); /* unused fields MUST be zero */
    xfer[0].tx_buf = (uintptr_t)tx1;
    xfer[0].rx_buf = (uintptr_t)rx1;
    xfer[0].len = LEN1;
    xfer[0].speed_hz = speed;
    xfer[0].bits_per_word = 8;
    xfer[1].tx_buf = (uintptr_t)tx2;
    xfer[1].rx_buf = (uintptr_t)rx2;
    xfer[1].len = LEN2;
    xfer[1].delay_usecs = 10; /* gap before CS is released */

    ret = ioctl(fd, SPI_IOC_MESSAGE(2), xfer);
    CHECK(ret == LEN1 + LEN2); /* returns total bytes transferred */

    if (memcmp(tx1, rx1, LEN1) || memcmp(tx2, rx2, LEN2)) {
        printf("FAIL rx != tx: rx starts %02x %02x %02x %02x, tx starts %02x %02x %02x %02x\n",
               rx1[0], rx1[1], rx1[2], rx1[3], tx1[0], tx1[1], tx1[2], tx1[3]);
        if (all_same(rx1, LEN1, 0x00) || all_same(rx1, LEN1, 0xff))
            printf("     MISO is constant: no MOSI->MISO jumper, or the MISO pad is not muxed\n"
                   "     (ti,pindir-d0-out-d1-in missing?)\n");
        close(fd);
        return 1;
    }
    printf("PASS loopback: %d + %d bytes in one message, rx == tx\n", LEN1, LEN2);
    close(fd);
    return 0;
}
