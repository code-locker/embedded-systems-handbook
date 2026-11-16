// SPDX-License-Identifier: GPL-2.0
/*
 * esh_i2c_bug.c — INTENTIONALLY BROKEN: the two I2C client bugs seen most
 *                 often in review
 *
 * Concept : bug=1  an SMBus transfer under a spinlock (or from a hard IRQ
 *                  handler). I2C transfers SLEEP: the adapter driver waits
 *                  for its completion interrupt, takes the bus mutex, may
 *                  runtime-resume the controller. Detected by
 *                  CONFIG_DEBUG_ATOMIC_SLEEP:
 *                  "BUG: sleeping function called from invalid context"
 *                  (i2c-stub does not sleep inside the transfer, but the
 *                  i2c core's rt_mutex_lock -> might_sleep() still fires.)
 *                  Fix: mutex instead of spinlock; threaded IRQ handler.
 *           bug=2  storing the return value of i2c_smbus_read_byte_data()
 *                  in a u8. It returns int: 0..255 = data, < 0 = -errno.
 *                  -ENXIO (no ACK) becomes 0xFA and the driver happily
 *                  reports "250". No tool catches this one; only review
 *                  and -Wconversion style warnings (W=3) hint at it.
 *           Bind it to an address with nothing behind it to see bug 2.
 * Build   : make -C linux-device-drivers/07-gpio-i2c-spi-drivers/kernel BUGS=1
 *           (NOT built by default, and not by `make drivers`)
 * Run     : ONLY in a VM / on the board, debug kernel for bug=1:
 *           sudo modprobe i2c-stub chip_addr=0x50
 *           sudo insmod esh_i2c_bug.ko bug=2
 *           echo esh_i2c_bug 0x51 | sudo tee /sys/bus/i2c/devices/i2c-N/new_device
 *           dmesg ; echo 0x51 | sudo tee /sys/bus/i2c/devices/i2c-N/delete_device
 * Read    : ../README.md  (section 7 "Common mistakes and pitfalls")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/spinlock.h>

static int bug = 1;
module_param(bug, int, 0444);
MODULE_PARM_DESC(bug, "1 = I2C transfer under a spinlock, 2 = errno stored in a u8");

/* raw_: atomic on every kernel, also PREEMPT_RT (see lesson 03) */
static DEFINE_RAW_SPINLOCK(bug_lock);

static int esh_i2c_bug_probe(struct i2c_client *client)
{
	u8 value;		/* BUG 2 lives here: should be int */
	int ret;

	switch (bug) {
	case 1:
		raw_spin_lock(&bug_lock);
		ret = i2c_smbus_read_byte_data(client, 0);	/* BUG 1: may sleep */
		raw_spin_unlock(&bug_lock);
		dev_info(&client->dev, "bug 1: read under spinlock returned %d; check dmesg for the BUG splat\n",
			 ret);
		break;
	case 2:
		value = i2c_smbus_read_byte_data(client, 0);	/* BUG 2: -ENXIO -> 0xfa */
		dev_info(&client->dev, "bug 2: \"register 0\" = %u (0x%02x) — if nothing ACKs at 0x%02x this is -ENXIO truncated\n",
			 value, value, client->addr);
		break;
	default:
		dev_info(&client->dev, "bug=%d: nothing to do (use 1 or 2)\n", bug);
	}
	return 0;
}

static const struct i2c_device_id esh_i2c_bug_id[] = {
	{ "esh_i2c_bug", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, esh_i2c_bug_id);

static struct i2c_driver esh_i2c_bug_driver = {
	.driver = { .name = "esh_i2c_bug" },
	.probe = esh_i2c_bug_probe,
	.id_table = esh_i2c_bug_id,
};
module_i2c_driver(esh_i2c_bug_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("embedded-systems-handbook");
MODULE_DESCRIPTION("Lesson 07: intentionally broken I2C client (sleep in atomic, errno in u8)");
