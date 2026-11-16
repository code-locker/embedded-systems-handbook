// SPDX-License-Identifier: GPL-2.0
/*
 * esh_sensor.c — regmap-i2c client driver for a FICTIONAL temperature sensor
 *
 * Concept : Real sensor drivers almost never call i2c_smbus_*() directly any
 *           more; they describe the register map once in a regmap_config and
 *           use regmap_read/regmap_write/regmap_update_bits. regmap picks the
 *           bus transfer (raw I2C or SMBus), locks, caches non-volatile
 *           registers so read-modify-write costs ONE bus write, and gives you
 *           /sys/kernel/debug/regmap/<dev>/registers for free. The same
 *           register map over SPI is esh_spi_sensor.c — only the regmap init
 *           line differs.
 *
 *           Register map (8-bit registers, 8-bit address, address 0x48):
 *             0x0F WHO_AM_I  RO        always 0xE5
 *             0x20 CTRL      RW        bit0 EN, bits3:1 RATE (0..7)
 *             0x27 STATUS    RO, vol.  bit0 DRDY (new sample available)
 *             0x28 DATA_L    RO, vol.  temperature, signed 16-bit little
 *             0x29 DATA_H    RO, vol.  endian, unit 0.01 degC
 *           With i2c-stub you play the chip: pre-load WHO_AM_I, STATUS and
 *           DATA with i2cset, then read /sys/.../temp.
 * Build   : make drivers   (or: make -C linux-device-drivers/07-gpio-i2c-spi-drivers/kernel)
 * Run     : VM:  sudo modprobe i2c-stub chip_addr=0x48    (bus N = "SMBus stub driver")
 *                sudo i2cset -y N 0x48 0x0f 0xe5          # WHO_AM_I
 *                sudo i2cset -y N 0x48 0x27 0x01          # STATUS.DRDY
 *                sudo i2cset -y N 0x48 0x28 0x34 ; sudo i2cset -y N 0x48 0x29 0x0c
 *                sudo insmod esh_sensor.ko rate=3
 *                echo esh_sensor 0x48 | sudo tee /sys/bus/i2c/devices/i2c-N/new_device
 *                cat /sys/bus/i2c/devices/N-0048/temp       # 31.24
 * Read    : ../README.md  (section 3.4 "regmap")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>

#define ESH_REG_WHO_AM_I	0x0f
#define ESH_REG_CTRL		0x20
#define   ESH_CTRL_EN		BIT(0)
#define   ESH_CTRL_RATE		GENMASK(3, 1)	/* see dsa/18-bit-manipulation */
#define ESH_REG_STATUS		0x27
#define   ESH_STATUS_DRDY	BIT(0)
#define ESH_REG_DATA_L		0x28
#define ESH_REG_DATA_H		0x29
#define ESH_WHO_AM_I_VALUE	0xe5

static unsigned int rate = 2;
module_param(rate, uint, 0444);
MODULE_PARM_DESC(rate, "initial output data rate code written to CTRL.RATE, 0..7");

struct esh_sensor {
	struct regmap *map;
	struct mutex lock;	/* STATUS-then-DATA must be one sequence */
};

/*
 * The access tables regmap uses to refuse nonsense (writing WHO_AM_I) and
 * to decide what it may cache. Volatile = "the chip changes it by itself":
 * never served from the cache. Everything else readable is cached after
 * the first read, so `regmap_update_bits(CTRL, ...)` does NOT need a bus
 * read first.
 */
static bool esh_readable(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case ESH_REG_WHO_AM_I:
	case ESH_REG_CTRL:
	case ESH_REG_STATUS:
	case ESH_REG_DATA_L:
	case ESH_REG_DATA_H:
		return true;
	default:
		return false;
	}
}

static bool esh_writeable(struct device *dev, unsigned int reg)
{
	return reg == ESH_REG_CTRL;
}

static bool esh_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case ESH_REG_STATUS:
	case ESH_REG_DATA_L:
	case ESH_REG_DATA_H:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config esh_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = ESH_REG_DATA_H,
	.readable_reg = esh_readable,
	.writeable_reg = esh_writeable,
	.volatile_reg = esh_volatile,
	.cache_type = REGCACHE_MAPLE,	/* REGCACHE_RBTREE before 6.4 */
};

/* ---- sysfs: /sys/bus/i2c/devices/N-0048/{temp,enable,rate} --------------- */

static ssize_t temp_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct esh_sensor *s = dev_get_drvdata(dev);
	unsigned int ctrl, status;
	u8 raw[2];
	int ret, centi;

	mutex_lock(&s->lock);
	ret = regmap_read(s->map, ESH_REG_CTRL, &ctrl);	/* cache hit, no bus */
	if (!ret && !(ctrl & ESH_CTRL_EN))
		ret = -ENODATA;			/* sensor is switched off */
	if (!ret)
		ret = regmap_read(s->map, ESH_REG_STATUS, &status); /* bus */
	if (!ret && !(status & ESH_STATUS_DRDY))
		ret = -EAGAIN;			/* no new sample yet */
	if (!ret)	/* both data bytes in ONE transfer: no torn L/H pair */
		ret = regmap_bulk_read(s->map, ESH_REG_DATA_L, raw, sizeof(raw));
	mutex_unlock(&s->lock);
	if (ret)
		return ret;

	centi = (s16)(raw[0] | raw[1] << 8);	/* little endian: L first */
	return sysfs_emit(buf, "%s%d.%02d\n", centi < 0 ? "-" : "",
			  abs(centi) / 100, abs(centi) % 100);
}
static DEVICE_ATTR_RO(temp);

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct esh_sensor *s = dev_get_drvdata(dev);
	unsigned int ctrl;
	int ret = regmap_read(s->map, ESH_REG_CTRL, &ctrl);

	return ret ? ret : sysfs_emit(buf, "%d\n", !!(ctrl & ESH_CTRL_EN));
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct esh_sensor *s = dev_get_drvdata(dev);
	bool on;
	int ret = kstrtobool(buf, &on);

	if (ret)
		return ret;
	/* read (from cache) - modify - write (one bus write) */
	ret = regmap_update_bits(s->map, ESH_REG_CTRL, ESH_CTRL_EN,
				 on ? ESH_CTRL_EN : 0);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(enable);

static ssize_t rate_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct esh_sensor *s = dev_get_drvdata(dev);
	unsigned int ctrl;
	int ret = regmap_read(s->map, ESH_REG_CTRL, &ctrl);

	return ret ? ret : sysfs_emit(buf, "%lu\n", FIELD_GET(ESH_CTRL_RATE, ctrl));
}

static ssize_t rate_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct esh_sensor *s = dev_get_drvdata(dev);
	unsigned int val;
	int ret = kstrtouint(buf, 0, &val);

	if (ret)
		return ret;
	if (val > FIELD_MAX(ESH_CTRL_RATE))
		return -ERANGE;
	ret = regmap_update_bits(s->map, ESH_REG_CTRL, ESH_CTRL_RATE,
				 FIELD_PREP(ESH_CTRL_RATE, val));
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(rate);

static struct attribute *esh_sensor_attrs[] = {
	&dev_attr_temp.attr,
	&dev_attr_enable.attr,
	&dev_attr_rate.attr,
	NULL
};
ATTRIBUTE_GROUPS(esh_sensor);	/* -> esh_sensor_groups, used in .driver */

/* ---- probe / remove ------------------------------------------------------ */

/* devm action: runs on unbind (or probe failure after it was added). */
static void esh_sensor_power_off(void *data)
{
	struct esh_sensor *s = data;

	regmap_update_bits(s->map, ESH_REG_CTRL, ESH_CTRL_EN, 0);
}

static int esh_sensor_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	unsigned int id, cached, hw;
	struct esh_sensor *s;
	int ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	mutex_init(&s->lock);

	/* The ONLY bus-specific line of the driver (cf. devm_regmap_init_spi). */
	s->map = devm_regmap_init_i2c(client, &esh_regmap_config);
	if (IS_ERR(s->map))
		return dev_err_probe(dev, PTR_ERR(s->map), "regmap init failed\n");

	/*
	 * Identify before touching anything: if something else answers at
	 * this address, writing CTRL could misconfigure a foreign chip.
	 */
	ret = regmap_read(s->map, ESH_REG_WHO_AM_I, &id);
	if (ret)
		return dev_err_probe(dev, ret, "no answer at 0x%02x\n", client->addr);
	if (id != ESH_WHO_AM_I_VALUE)
		return dev_err_probe(dev, -ENODEV,
				     "WHO_AM_I=0x%02x, expected 0x%02x (i2c-stub: i2cset -y %d 0x%02x 0x0f 0xe5)\n",
				     id, ESH_WHO_AM_I_VALUE, client->adapter->nr,
				     client->addr);

	ret = regmap_update_bits(s->map, ESH_REG_CTRL,
				 ESH_CTRL_EN | ESH_CTRL_RATE,
				 ESH_CTRL_EN | FIELD_PREP(ESH_CTRL_RATE, rate));
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, esh_sensor_power_off, s);
	if (ret)
		return ret;

	/*
	 * Self-test: CTRL from the cache vs CTRL read from the chip. Bypass
	 * mode sends the read to the bus even for a cached register.
	 */
	ret = regmap_read(s->map, ESH_REG_CTRL, &cached);
	if (!ret) {
		regcache_cache_bypass(s->map, true);
		ret = regmap_read(s->map, ESH_REG_CTRL, &hw);
		regcache_cache_bypass(s->map, false);
	}
	if (ret)
		dev_err(dev, "FAIL selftest: CTRL read: %d\n", ret);
	else if (cached != hw)
		dev_err(dev, "FAIL selftest: CTRL cache 0x%02x != chip 0x%02x\n",
			cached, hw);
	else
		dev_info(dev, "PASS selftest: WHO_AM_I 0x%02x, CTRL 0x%02x (EN=1 RATE=%u) in cache and chip\n",
			 id, hw, rate);

	i2c_set_clientdata(client, s);
	return 0;
}

static const struct of_device_id esh_sensor_of_match[] = {
	{ .compatible = "esh,temp-sensor" },
	{ }
};
MODULE_DEVICE_TABLE(of, esh_sensor_of_match);

static const struct i2c_device_id esh_sensor_id[] = {
	{ "esh_sensor", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, esh_sensor_id);

static struct i2c_driver esh_sensor_driver = {
	.driver = {
		.name = "esh_sensor",
		.of_match_table = esh_sensor_of_match,
		/* created BEFORE the uevent, so udev rules see them; no race */
		.dev_groups = esh_sensor_groups,
	},
	.probe = esh_sensor_probe,
	.id_table = esh_sensor_id,
};

static int __init esh_sensor_init(void)
{
	if (rate > FIELD_MAX(ESH_CTRL_RATE)) {
		pr_err("rate=%u out of range 0..%lu\n", rate,
		       FIELD_MAX(ESH_CTRL_RATE));
		return -EINVAL;
	}
	return i2c_add_driver(&esh_sensor_driver);
}
module_init(esh_sensor_init);

static void __exit esh_sensor_exit(void)
{
	i2c_del_driver(&esh_sensor_driver);
}
module_exit(esh_sensor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("embedded-systems-handbook");
MODULE_DESCRIPTION("Lesson 07: regmap-i2c driver for a fictional temperature sensor");
