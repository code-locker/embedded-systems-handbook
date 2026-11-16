// SPDX-License-Identifier: GPL-2.0
/*
 * esh_spi_sensor.c — SPI client driver (regmap-spi) for the same FICTIONAL
 *                    temperature sensor as esh_sensor.c, plus a loopback test
 *
 * Concept : An spi_driver binds to an spi_device that the controller driver
 *           (TDA4VM McSPI: spi-omap2-mcspi) created from a DT child node.
 *           The register map is identical to esh_sensor.c; on SPI the first
 *           byte of each transaction is the register address with bit 7 = 1
 *           for a read (the common sensor convention, regmap's
 *           read_flag_mask):
 *
 *             CS  ‾‾\___________________________________/‾‾
 *             MOSI    [1|addr 7 bits][ don't care ........]
 *             MISO    [ ignored     ][ data byte(s) .......]
 *                      byte 0          byte 1..n  (full duplex, mode 0)
 *
 *           With loopback=1 (a wire from MOSI to MISO, no sensor) the probe
 *           instead runs a full-duplex spi_sync() test (rx must equal tx)
 *           and shows what spi_write_then_read() sees on a looped bus.
 *           There is no SPI simulator for VMs: this driver is build-tested on
 *           the host and run on the board.
 * Build   : make drivers   (or: make -C linux-device-drivers/07-gpio-i2c-spi-drivers/kernel)
 * Run     : Board: load dts/esh-spi7-device.dtso (SPI7 on P9.28 CS0, P9.31 SCLK,
 *                  P9.30 MOSI, P9.29 MISO), jumper P9.30 <-> P9.29, then
 *                  sudo insmod esh_spi_sensor.ko loopback=1 ; dmesg | grep esh_spi
 * Read    : ../README.md  (section 3.5 "SPI client drivers")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>

#define ESH_REG_WHO_AM_I	0x0f
#define ESH_REG_CTRL		0x20
#define   ESH_CTRL_EN		BIT(0)
#define ESH_REG_STATUS		0x27
#define   ESH_STATUS_DRDY	BIT(0)
#define ESH_REG_DATA_L		0x28
#define ESH_REG_DATA_H		0x29
#define ESH_WHO_AM_I_VALUE	0xe5
#define ESH_SPI_READ		BIT(7)	/* address byte bit 7: 1 = read */
#define LOOP_LEN		32

static bool loopback;
module_param(loopback, bool, 0444);
MODULE_PARM_DESC(loopback, "no sensor: MOSI is wired to MISO, run the loopback self-test instead");

static unsigned int max_hz = 1000000;
module_param(max_hz, uint, 0444);
MODULE_PARM_DESC(max_hz, "SCLK limit in Hz, 1000..50000000 (lower than DT spi-max-frequency wins)");

struct esh_spi_sensor {
	struct spi_device *spi;
	struct regmap *map;
	struct mutex lock;	/* STATUS-then-DATA must be one sequence */
};

static bool esh_volatile(struct device *dev, unsigned int reg)
{
	return reg == ESH_REG_STATUS || reg == ESH_REG_DATA_L ||
	       reg == ESH_REG_DATA_H;
}

static bool esh_writeable(struct device *dev, unsigned int reg)
{
	return reg == ESH_REG_CTRL;
}

static const struct regmap_config esh_spi_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.read_flag_mask = ESH_SPI_READ,	/* OR-ed into the address on reads */
	.max_register = ESH_REG_DATA_H,
	.writeable_reg = esh_writeable,
	.volatile_reg = esh_volatile,
	.cache_type = REGCACHE_MAPLE,
};

/* ---- sysfs (same meaning as esh_sensor.c) -------------------------------- */

static ssize_t temp_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct esh_spi_sensor *s = dev_get_drvdata(dev);
	unsigned int ctrl, status;
	u8 raw[2];
	int ret, centi;

	mutex_lock(&s->lock);
	ret = regmap_read(s->map, ESH_REG_CTRL, &ctrl);
	if (!ret && !(ctrl & ESH_CTRL_EN))
		ret = -ENODATA;
	if (!ret)
		ret = regmap_read(s->map, ESH_REG_STATUS, &status);
	if (!ret && !(status & ESH_STATUS_DRDY))
		ret = -EAGAIN;
	if (!ret)	/* one CS-low burst: address auto-increments in the chip */
		ret = regmap_bulk_read(s->map, ESH_REG_DATA_L, raw, sizeof(raw));
	mutex_unlock(&s->lock);
	if (ret)
		return ret;

	centi = (s16)(raw[0] | raw[1] << 8);
	return sysfs_emit(buf, "%s%d.%02d\n", centi < 0 ? "-" : "",
			  abs(centi) / 100, abs(centi) % 100);
}
static DEVICE_ATTR_RO(temp);

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct esh_spi_sensor *s = dev_get_drvdata(dev);
	bool on;
	int ret = kstrtobool(buf, &on);

	if (ret)
		return ret;
	ret = regmap_update_bits(s->map, ESH_REG_CTRL, ESH_CTRL_EN,
				 on ? ESH_CTRL_EN : 0);
	return ret ? ret : count;
}

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct esh_spi_sensor *s = dev_get_drvdata(dev);
	unsigned int ctrl;
	int ret = regmap_read(s->map, ESH_REG_CTRL, &ctrl);

	return ret ? ret : sysfs_emit(buf, "%d\n", !!(ctrl & ESH_CTRL_EN));
}
static DEVICE_ATTR_RW(enable);

static struct attribute *esh_spi_sensor_attrs[] = {
	&dev_attr_temp.attr,
	&dev_attr_enable.attr,
	NULL
};
ATTRIBUTE_GROUPS(esh_spi_sensor);

/* ---- loopback self-test --------------------------------------------------- */

/*
 * spi_message = one CS-asserted transaction made of spi_transfers.
 * Here: one full-duplex transfer; every clock shifts one bit out on MOSI
 * and one bit in on MISO at the same time. With MOSI wired to MISO, the
 * byte received at clock n is the byte sent at clock n: rx == tx.
 *
 * Buffers come from kmalloc, NOT the stack: the controller may DMA them
 * (spi-omap2-mcspi switches to DMA for longer transfers) and stack memory
 * is not DMA-safe (vmap'ed stacks, cache-line sharing).
 */
static void esh_spi_loopback_test(struct esh_spi_sensor *s)
{
	struct device *dev = &s->spi->dev;
	struct spi_transfer xfer = { };
	struct spi_message msg;
	u8 *tx, *rx, cmd, val = 0xaa;
	int ret, i;

	tx = kmalloc(LOOP_LEN, GFP_KERNEL);
	rx = kzalloc(LOOP_LEN, GFP_KERNEL);
	if (!tx || !rx)
		goto out;
	for (i = 0; i < LOOP_LEN; i++)
		tx[i] = 0x5a ^ (i * 37);	/* not all-0, not all-1 */

	xfer.tx_buf = tx;
	xfer.rx_buf = rx;
	xfer.len = LOOP_LEN;
	xfer.speed_hz = 0;		/* 0 = use spi->max_speed_hz */
	spi_message_init_with_transfers(&msg, &xfer, 1);

	ret = spi_sync(s->spi, &msg);	/* sleeps until done: process ctx only */
	if (ret)
		dev_err(dev, "FAIL loopback: spi_sync: %d\n", ret);
	else if (memcmp(tx, rx, LOOP_LEN))
		dev_err(dev, "FAIL loopback: rx %*ph... != tx %*ph... (MOSI-MISO jumper missing? wrong MISO pin mux?)\n",
			8, rx, 8, tx);
	else
		dev_info(dev, "PASS loopback: %d bytes full duplex at %u Hz, rx == tx\n",
			 LOOP_LEN, s->spi->max_speed_hz);

	/*
	 * spi_write_then_read(): half-duplex helper, copies to an internal
	 * DMA-safe buffer so stack variables are fine here. During the read
	 * phase it clocks out dummy bytes (zeros), so on a looped bus the
	 * "register value" read back is that dummy byte.
	 */
	cmd = ESH_REG_WHO_AM_I | ESH_SPI_READ;
	ret = spi_write_then_read(s->spi, &cmd, 1, &val, 1);
	if (ret)
		dev_err(dev, "FAIL loopback: spi_write_then_read: %d\n", ret);
	else
		dev_info(dev, "INFO loopback: WHO_AM_I read on a looped bus = 0x%02x (the dummy byte, not 0x%02x)\n",
			 val, ESH_WHO_AM_I_VALUE);
out:
	kfree(rx);
	kfree(tx);
}

/* ---- probe ---------------------------------------------------------------- */

static void esh_spi_sensor_power_off(void *data)
{
	struct esh_spi_sensor *s = data;

	regmap_update_bits(s->map, ESH_REG_CTRL, ESH_CTRL_EN, 0);
}

static int esh_spi_sensor_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct esh_spi_sensor *s;
	unsigned int id;
	int ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->spi = spi;
	mutex_init(&s->lock);
	spi_set_drvdata(spi, s);

	/*
	 * Mode (CPOL/CPHA), CS polarity and the max clock come from DT
	 * (spi-cpol, spi-cpha, spi-cs-high, spi-max-frequency). The driver
	 * only tightens what it knows about the chip, then calls spi_setup()
	 * so the controller validates and applies it.
	 */
	spi->bits_per_word = 8;
	if (!spi->max_speed_hz || spi->max_speed_hz > max_hz)
		spi->max_speed_hz = max_hz;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "spi_setup failed\n");

	s->map = devm_regmap_init_spi(spi, &esh_spi_regmap_config);
	if (IS_ERR(s->map))
		return dev_err_probe(dev, PTR_ERR(s->map), "regmap init failed\n");

	dev_info(dev, "on %s CS%d, mode %u, %u Hz\n", dev_name(&spi->controller->dev),
		 spi_get_chipselect(spi, 0), (unsigned int)(spi->mode & SPI_MODE_X_MASK),
		 spi->max_speed_hz);

	if (loopback) {
		esh_spi_loopback_test(s);
		return 0;	/* stay bound so the test can be repeated by rebinding */
	}

	ret = regmap_read(s->map, ESH_REG_WHO_AM_I, &id);
	if (ret)
		return dev_err_probe(dev, ret, "WHO_AM_I read failed\n");
	if (id != ESH_WHO_AM_I_VALUE)
		return dev_err_probe(dev, -ENODEV,
				     "WHO_AM_I=0x%02x, expected 0x%02x (no sensor? try loopback=1)\n",
				     id, ESH_WHO_AM_I_VALUE);

	ret = regmap_update_bits(s->map, ESH_REG_CTRL, ESH_CTRL_EN, ESH_CTRL_EN);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, esh_spi_sensor_power_off, s);
	if (ret)
		return ret;
	dev_info(dev, "PASS probe: WHO_AM_I 0x%02x, sensor enabled\n", id);
	return 0;
}

static const struct of_device_id esh_spi_sensor_of_match[] = {
	{ .compatible = "esh,spi-temp-sensor" },
	{ }
};
MODULE_DEVICE_TABLE(of, esh_spi_sensor_of_match);

/*
 * The SPI core builds the modalias "spi:<name>" from the compatible
 * WITHOUT vendor and warns at register time if no id_table entry matches
 * ("SPI driver esh_spi_sensor has no spi_device_id for esh,spi-temp-sensor").
 */
static const struct spi_device_id esh_spi_sensor_id[] = {
	{ "spi-temp-sensor", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, esh_spi_sensor_id);

static struct spi_driver esh_spi_sensor_driver = {
	.driver = {
		.name = "esh_spi_sensor",
		.of_match_table = esh_spi_sensor_of_match,
		.dev_groups = esh_spi_sensor_groups,
	},
	.probe = esh_spi_sensor_probe,
	.id_table = esh_spi_sensor_id,
	/* no .remove: all devm_; (spi .remove returns void since 5.18) */
};

static int __init esh_spi_sensor_init(void)
{
	if (max_hz < 1000 || max_hz > 50000000) {
		pr_err("max_hz=%u out of range 1000..50000000\n", max_hz);
		return -EINVAL;
	}
	return spi_register_driver(&esh_spi_sensor_driver);
}
module_init(esh_spi_sensor_init);

static void __exit esh_spi_sensor_exit(void)
{
	spi_unregister_driver(&esh_spi_sensor_driver);
}
module_exit(esh_spi_sensor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("embedded-systems-handbook");
MODULE_DESCRIPTION("Lesson 07: regmap-spi driver for a fictional sensor, with MOSI-MISO loopback test");
