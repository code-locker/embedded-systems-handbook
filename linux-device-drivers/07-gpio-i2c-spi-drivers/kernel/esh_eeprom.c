// SPDX-License-Identifier: GPL-2.0
/*
 * esh_eeprom.c — I2C client driver for a 24C02-style EEPROM (256 bytes,
 *                8-byte pages, 8-bit word address), exported through nvmem
 *
 * Concept : The classic "first I2C driver": an i2c_driver that binds to an
 *           i2c_client (from DT or from new_device in sysfs), checks what the
 *           ADAPTER can do (i2c_check_functionality) and picks the best
 *           transfer method — raw i2c_transfer() with a repeated start, the
 *           SMBus "I2C block" emulation, or byte-at-a-time SMBus — exactly
 *           like the real at24 driver. Data reaches user space through the
 *           nvmem framework (/sys/bus/nvmem/devices/esh_eeprom0/nvmem), not a
 *           hand-written char device. At probe it reads the first 16 bytes
 *           twice (fast path vs byte-by-byte) and prints PASS/FAIL. It never
 *           WRITES at probe: an EEPROM may hold a board ID or MAC address.
 * Build   : make drivers   (or: make -C linux-device-drivers/07-gpio-i2c-spi-drivers/kernel)
 * Run     : VM:    sudo modprobe i2c-stub chip_addr=0x50     (bus N = "SMBus stub driver")
 *                  sudo i2cset -y N 0x50 0x00 0x45 ; sudo i2cset -y N 0x50 0x01 0x53
 *                  sudo insmod esh_eeprom.ko
 *                  echo esh_eeprom 0x50 | sudo tee /sys/bus/i2c/devices/i2c-N/new_device
 *                  sudo hexdump -C /sys/bus/nvmem/devices/esh_eeprom0/nvmem | head
 *           Board: 24C02 module on P9.19 (SCL) / P9.20 (SDA) = /dev/i2c-2,
 *                  dts/esh-i2c2-devices.dtso instantiates it from DT.
 * Read    : ../README.md  (section 3.3 "I2C client drivers")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-provider.h>
#include <linux/property.h>

#define EE_SIZE		256	/* 24C02: 2 Kbit; the word address is ONE byte */
#define EE_WRITE_TMO_MS	25	/* datasheet tWR is 5 ms; allow generous slack */
#define EE_SELFTEST_LEN	16

/* "page_size" is taken: mm.h has an inline page_size(struct page *). */
static unsigned int ee_page = 8;
module_param_named(page_size, ee_page, uint, 0444);
MODULE_PARM_DESC(page_size, "write page size in bytes, power of two 1..16 (24C02: 8)");

/*
 * How we talk to the chip. The adapter decides, not us:
 *   - a real I2C controller (TDA4VM main_i2cN, omap-i2c driver) can do any
 *     message sequence -> I2C_FUNC_I2C -> raw i2c_transfer()
 *   - SMBus-only controllers (many PC chipsets, and i2c-stub!) cannot do
 *     an arbitrary repeated-start read, but most emulate "I2C block read"
 *   - the lowest common denominator is one SMBus "read byte data" per byte
 */
enum ee_mode { EE_RAW_I2C, EE_SMBUS_I2C_BLOCK, EE_SMBUS_BYTE };

static const char *const ee_mode_name[] = {
	[EE_RAW_I2C]		= "raw i2c_transfer (repeated start)",
	[EE_SMBUS_I2C_BLOCK]	= "SMBus I2C-block emulation",
	[EE_SMBUS_BYTE]		= "SMBus byte data, one byte per transfer",
};

struct esh_eeprom {
	struct i2c_client *client;
	struct mutex lock;		/* one transaction sequence at a time */
	enum ee_mode mode;
	bool smbus_block_write;		/* adapter can do I2C-block writes */
	bool read_only;
};

/*
 * Random read with a repeated start (24Cxx datasheet "sequential read"):
 *
 *   S | 0x50 W | A | off | A | Sr | 0x50 R | A | d0 A d1 A ... dn NA | P
 *     \____ msg[0]: write 1 byte ____/ \______ msg[1]: read len _______/
 *
 * Both messages go out in ONE i2c_transfer() call, so no other master can
 * sneak in between setting the address pointer and reading (the bus is
 * never released: Sr instead of P+S).
 */
static int ee_read_raw(struct esh_eeprom *ee, u8 off, u8 *buf, size_t len)
{
	struct i2c_msg msgs[2] = {
		{ .addr = ee->client->addr, .flags = 0, .len = 1, .buf = &off },
		{ .addr = ee->client->addr, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret;

	ret = i2c_transfer(ee->client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	return ret == ARRAY_SIZE(msgs) ? (int)len : -EIO;
}

/* Read up to len bytes at off; returns bytes read (may be < len) or -errno. */
static int ee_read_chunk(struct esh_eeprom *ee, unsigned int off, u8 *buf,
			 size_t len)
{
	int ret;

	switch (ee->mode) {
	case EE_RAW_I2C:
		/* Adapters have a max message length; 32 is safe everywhere. */
		return ee_read_raw(ee, off, buf, min_t(size_t, len, 32));
	case EE_SMBUS_I2C_BLOCK:
		/* SMBus caps a block at I2C_SMBUS_BLOCK_MAX (32) bytes. */
		return i2c_smbus_read_i2c_block_data(ee->client, off,
				min_t(size_t, len, I2C_SMBUS_BLOCK_MAX), buf);
	case EE_SMBUS_BYTE:
	default:
		ret = i2c_smbus_read_byte_data(ee->client, off);
		if (ret < 0)
			return ret;	/* negative = errno, NOT data */
		buf[0] = ret;
		return 1;
	}
}

static int ee_write_chunk(struct esh_eeprom *ee, unsigned int off,
			  const u8 *buf, size_t len)
{
	u8 tmp[1 + 16];		/* word address + one page (page_size <= 16) */
	struct i2c_msg msg = { .addr = ee->client->addr, .flags = 0, .buf = tmp };
	int ret;

	switch (ee->mode) {
	case EE_RAW_I2C:
		/* Page write: S | 0x50 W | A | off | A | d0 | A ... | P */
		tmp[0] = off;
		memcpy(&tmp[1], buf, len);
		msg.len = 1 + len;
		ret = i2c_transfer(ee->client->adapter, &msg, 1);
		if (ret < 0)
			return ret;
		return ret == 1 ? (int)len : -EIO;
	default:
		if (ee->smbus_block_write) {
			ret = i2c_smbus_write_i2c_block_data(ee->client, off,
							     len, buf);
			return ret < 0 ? ret : (int)len;
		}
		ret = i2c_smbus_write_byte_data(ee->client, off, buf[0]);
		return ret < 0 ? ret : 1;
	}
}

/*
 * After a write the EEPROM spends ~5 ms programming its cells and NACKs
 * its own address meanwhile (the i2c core then returns -ENXIO/-EREMOTEIO,
 * depending on the controller driver). Instead of a fixed msleep(5) we
 * retry until it ACKs again or EE_WRITE_TMO_MS passes — "ACK polling",
 * what the datasheet recommends and what at24 does.
 */
static int ee_write_retry(struct esh_eeprom *ee, unsigned int off,
			  const u8 *buf, size_t len)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(EE_WRITE_TMO_MS);
	int ret;

	do {
		ret = ee_write_chunk(ee, off, buf, len);
		if (ret >= 0)
			return ret;
		usleep_range(1000, 1500);	/* sleeping is fine: process context */
	} while (time_before(jiffies, timeout));
	return ret;
}

/* ---- nvmem callbacks: called in process context, may sleep -------------- */

static int esh_eeprom_nvmem_read(void *priv, unsigned int off, void *val,
				 size_t count)
{
	struct esh_eeprom *ee = priv;
	u8 *buf = val;
	int ret = 0;

	/* nvmem core already clipped off+count to the size we registered. */
	mutex_lock(&ee->lock);
	while (count) {
		ret = ee_read_chunk(ee, off, buf, count);
		if (ret < 0)
			break;
		off += ret;
		buf += ret;
		count -= ret;
		ret = 0;
	}
	mutex_unlock(&ee->lock);
	return ret;			/* nvmem wants 0 or -errno */
}

static int esh_eeprom_nvmem_write(void *priv, unsigned int off, void *val,
				  size_t count)
{
	struct esh_eeprom *ee = priv;
	const u8 *buf = val;
	int ret = 0;

	mutex_lock(&ee->lock);
	while (count) {
		/*
		 * A page write must not cross a page boundary: the chip's
		 * address counter only increments the low bits and would
		 * WRAP to the start of the same page, overwriting it.
		 *   page_size=8, off=6, count=5 -> write 2 (6,7), then 3 (8..10)
		 */
		size_t room = ee_page - (off & (ee_page - 1));
		size_t n = min(count, room);

		ret = ee_write_retry(ee, off, buf, n);
		if (ret < 0)
			break;
		off += ret;
		buf += ret;
		count -= ret;
		ret = 0;
	}
	mutex_unlock(&ee->lock);
	return ret;
}

/* ---- probe-time self-test: read-only ------------------------------------ */

static void esh_eeprom_selftest(struct esh_eeprom *ee)
{
	struct device *dev = &ee->client->dev;
	u8 fast[EE_SELFTEST_LEN], slow[EE_SELFTEST_LEN];
	int ret, i;

	ret = esh_eeprom_nvmem_read(ee, 0, fast, sizeof(fast));
	if (ret) {
		dev_err(dev, "FAIL selftest: read via %s: %d\n",
			ee_mode_name[ee->mode], ret);
		return;
	}
	if (!i2c_check_functionality(ee->client->adapter,
				     I2C_FUNC_SMBUS_READ_BYTE_DATA)) {
		dev_info(dev, "INFO selftest: adapter has no SMBus byte read, comparison skipped\n");
		return;
	}
	for (i = 0; i < EE_SELFTEST_LEN; i++) {
		ret = i2c_smbus_read_byte_data(ee->client, i);
		if (ret < 0) {
			dev_err(dev, "FAIL selftest: byte read @%d: %d\n", i, ret);
			return;
		}
		slow[i] = ret;
	}
	if (memcmp(fast, slow, sizeof(fast)))
		dev_err(dev, "FAIL selftest: %s and byte-by-byte reads differ\n",
			ee_mode_name[ee->mode]);
	else
		dev_info(dev, "PASS selftest: first %d bytes identical via both paths: %*ph\n",
			 EE_SELFTEST_LEN, EE_SELFTEST_LEN, fast);
}

/* ---- probe / remove ------------------------------------------------------ */

static int esh_eeprom_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct i2c_adapter *adap = client->adapter;
	struct nvmem_config cfg = { };
	struct nvmem_device *nvmem;
	struct esh_eeprom *ee;

	/* devm_: freed automatically when the device unbinds or probe fails. */
	ee = devm_kzalloc(dev, sizeof(*ee), GFP_KERNEL);
	if (!ee)
		return -ENOMEM;
	ee->client = client;
	mutex_init(&ee->lock);

	if (i2c_check_functionality(adap, I2C_FUNC_I2C))
		ee->mode = EE_RAW_I2C;
	else if (i2c_check_functionality(adap, I2C_FUNC_SMBUS_READ_I2C_BLOCK))
		ee->mode = EE_SMBUS_I2C_BLOCK;
	else if (i2c_check_functionality(adap, I2C_FUNC_SMBUS_READ_BYTE_DATA))
		ee->mode = EE_SMBUS_BYTE;
	else
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "adapter %s cannot even do SMBus byte reads\n",
				     adap->name);
	ee->smbus_block_write =
		i2c_check_functionality(adap, I2C_FUNC_SMBUS_WRITE_I2C_BLOCK);

	/* DT: "read-only;" in the node. Works for ACPI/swnode properties too. */
	ee->read_only = device_property_read_bool(dev, "read-only");
	if (!ee->read_only && ee->mode != EE_RAW_I2C && !ee->smbus_block_write &&
	    !i2c_check_functionality(adap, I2C_FUNC_SMBUS_WRITE_BYTE_DATA))
		ee->read_only = true;

	cfg.name = "esh_eeprom";
	cfg.id = NVMEM_DEVID_AUTO;	/* esh_eeprom0, esh_eeprom1, ... */
	cfg.dev = dev;
	cfg.owner = THIS_MODULE;
	cfg.type = NVMEM_TYPE_EEPROM;
	cfg.read_only = ee->read_only;
	cfg.size = EE_SIZE;
	cfg.word_size = 1;
	cfg.stride = 1;
	cfg.reg_read = esh_eeprom_nvmem_read;
	cfg.reg_write = esh_eeprom_nvmem_write;
	cfg.priv = ee;

	nvmem = devm_nvmem_register(dev, &cfg);
	if (IS_ERR(nvmem))
		return dev_err_probe(dev, PTR_ERR(nvmem), "nvmem register failed\n");

	i2c_set_clientdata(client, ee);
	dev_info(dev, "%d bytes at 0x%02x on %s, %s%s, page %u\n",
		 EE_SIZE, client->addr, adap->name, ee_mode_name[ee->mode],
		 ee->read_only ? ", read-only" : "", ee_page);

	esh_eeprom_selftest(ee);
	return 0;
}

/*
 * No .remove needed: everything was devm_-allocated and is released in
 * reverse order after unbind (nvmem unregister first, then the struct).
 */

/* DT match: compatible = "esh,24c02" in a node under an I2C bus node. */
static const struct of_device_id esh_eeprom_of_match[] = {
	{ .compatible = "esh,24c02" },
	{ }
};
MODULE_DEVICE_TABLE(of, esh_eeprom_of_match);

/*
 * Name match: used by new_device ("esh_eeprom 0x50") and board files. The
 * i2c core also falls back to it for DT nodes (compatible minus vendor).
 */
static const struct i2c_device_id esh_eeprom_id[] = {
	{ "esh_eeprom", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, esh_eeprom_id);

static struct i2c_driver esh_eeprom_driver = {
	.driver = {
		.name = "esh_eeprom",
		.of_match_table = esh_eeprom_of_match,
	},
	.probe = esh_eeprom_probe,	/* single-argument probe since 6.3 */
	.id_table = esh_eeprom_id,
};

static int __init esh_eeprom_init(void)
{
	if (!ee_page || ee_page > 16 || (ee_page & (ee_page - 1))) {
		pr_err("page_size=%u: must be a power of two 1..16\n", ee_page);
		return -EINVAL;
	}
	return i2c_add_driver(&esh_eeprom_driver);
}
module_init(esh_eeprom_init);

static void __exit esh_eeprom_exit(void)
{
	i2c_del_driver(&esh_eeprom_driver);
}
module_exit(esh_eeprom_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("embedded-systems-handbook");
MODULE_DESCRIPTION("Lesson 07: 24C02-style I2C EEPROM client driver exported via nvmem");
