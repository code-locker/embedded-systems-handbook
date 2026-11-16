// SPDX-License-Identifier: GPL-2.0
/*
 * esh_gpio_demo.c — GPIO CONSUMER driver: an LED output and a button input
 *                   with an interrupt, using the descriptor (gpiod) API
 *
 * Concept : A driver never hard-codes "GPIO number 412". It asks for a
 *           FUNCTION ("led", "button") and firmware says which line that is:
 *             DT:      led-gpios = <&main_gpio0 4 GPIO_ACTIVE_HIGH>;
 *             no DT:   a gpiod_lookup_table (board file / this module's
 *                      sim_chip= mode, used with gpio-sim in a VM)
 *           devm_gpiod_get() returns a struct gpio_desc *. Values are
 *           LOGICAL: 1 = "active". If the line is wired active-low, the DT
 *           flag inverts it inside gpiolib and the driver code is unchanged.
 *           The button's edge interrupt comes from gpiod_to_irq() and is
 *           handled in a threaded handler (it may sleep: needed when the
 *           GPIO sits on an I2C expander, and the norm on PREEMPT_RT).
 *           Self-test at probe: drive the LED 1/0 and read it back.
 * Build   : make drivers   (or: make -C linux-device-drivers/07-gpio-i2c-spi-drivers/kernel)
 * Run     : VM with gpio-sim (see ../README.md section 8 for the configfs steps):
 *             sudo insmod esh_gpio_demo.ko sim_chip=esh-sim
 *             cat /sys/devices/platform/esh-gpio-demo/{led,button,presses}
 *           Board: a DT node (README section 3.1) with compatible "esh,gpio-demo".
 * Read    : ../README.md  (section 3.1 "GPIO")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define DRV_NAME "esh-gpio-demo"

static char *sim_chip;
module_param(sim_chip, charp, 0444);
MODULE_PARM_DESC(sim_chip, "no-DT test mode: label of a GPIO chip (e.g. a gpio-sim bank) to use");

static unsigned int led_line;
module_param(led_line, uint, 0444);
MODULE_PARM_DESC(led_line, "sim_chip mode: line offset for the LED (default 0)");

static unsigned int button_line = 1;
module_param(button_line, uint, 0444);
MODULE_PARM_DESC(button_line, "sim_chip mode: line offset for the button (default 1)");

static bool led_active_low;
module_param(led_active_low, bool, 0444);
MODULE_PARM_DESC(led_active_low, "sim_chip mode: LED wired active-low (the DT GPIO_ACTIVE_LOW flag)");

struct esh_gpio {
	struct gpio_desc *led;
	struct gpio_desc *button;	/* optional: may be NULL */
	atomic_t presses;
};

/*
 * Threaded IRQ handler (runs in a kthread "irq/NN-esh-gpio-demo").
 * Logical value: 1 = pressed, whatever the wiring, because the button was
 * requested with its active-low flag from firmware.
 */
static irqreturn_t esh_button_thread(int irq, void *data)
{
	struct esh_gpio *g = data;
	int pressed = gpiod_get_value_cansleep(g->button);

	if (pressed < 0)
		return IRQ_NONE;
	if (pressed)
		atomic_inc(&g->presses);
	gpiod_set_value_cansleep(g->led, pressed);	/* LED mirrors button */
	return IRQ_HANDLED;
}

/* ---- sysfs: /sys/devices/platform/esh-gpio-demo/{led,button,presses} ------ */

static ssize_t led_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct esh_gpio *g = dev_get_drvdata(dev);
	int v = gpiod_get_value_cansleep(g->led);

	return v < 0 ? v : sysfs_emit(buf, "%d\n", v);
}

static ssize_t led_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct esh_gpio *g = dev_get_drvdata(dev);
	bool on;
	int ret = kstrtobool(buf, &on);

	if (ret)
		return ret;
	/* _cansleep: we are in process context, so ANY gpio chip is fine */
	gpiod_set_value_cansleep(g->led, on);
	return count;
}
static DEVICE_ATTR_RW(led);

static ssize_t button_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct esh_gpio *g = dev_get_drvdata(dev);
	int v;

	if (!g->button)
		return -ENODEV;
	v = gpiod_get_value_cansleep(g->button);
	return v < 0 ? v : sysfs_emit(buf, "%d\n", v);
}
static DEVICE_ATTR_RO(button);

static ssize_t presses_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct esh_gpio *g = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", atomic_read(&g->presses));
}
static DEVICE_ATTR_RO(presses);

static struct attribute *esh_gpio_attrs[] = {
	&dev_attr_led.attr,
	&dev_attr_button.attr,
	&dev_attr_presses.attr,
	NULL
};
ATTRIBUTE_GROUPS(esh_gpio);

/* ---- probe ---------------------------------------------------------------- */

static void esh_gpio_selftest(struct device *dev, struct esh_gpio *g)
{
	int on, off;

	gpiod_set_value_cansleep(g->led, 1);
	on = gpiod_get_value_cansleep(g->led);
	gpiod_set_value_cansleep(g->led, 0);
	off = gpiod_get_value_cansleep(g->led);

	/*
	 * Reading an OUTPUT back depends on the controller: gpio-davinci
	 * (TDA4VM) reads the pad's input buffer, which is off if the pinmux
	 * entry says PIN_OUTPUT. Use PIN_INPUT (buffer on) for GPIO pads.
	 */
	if (on == 1 && off == 0)
		dev_info(dev, "PASS selftest: LED set 1 -> read 1, set 0 -> read 0 (%s)\n",
			 gpiod_is_active_low(g->led) ? "active-low line, logical values" :
						       "active-high line");
	else
		dev_err(dev, "FAIL selftest: LED read back %d/%d, expected 1/0 (pad input buffer disabled in pinmux?)\n",
			on, off);
}

static int esh_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct esh_gpio *g;
	int irq, ret;

	g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;
	atomic_set(&g->presses, 0);

	/*
	 * "led" -> property led-gpios (DT) or con_id "led" (lookup table).
	 * GPIOD_OUT_LOW: configure as output, logical 0, atomically at
	 * request time, so the LED never glitches on.
	 * -EPROBE_DEFER if the gpio controller has not probed yet:
	 * dev_err_probe() stays silent for that and the core retries later.
	 */
	g->led = devm_gpiod_get(dev, "led", GPIOD_OUT_LOW);
	if (IS_ERR(g->led))
		return dev_err_probe(dev, PTR_ERR(g->led), "no led gpio\n");

	/* _optional: NULL (not an error) when the board has no button. */
	g->button = devm_gpiod_get_optional(dev, "button", GPIOD_IN);
	if (IS_ERR(g->button))
		return dev_err_probe(dev, PTR_ERR(g->button), "bad button gpio\n");

	gpiod_set_consumer_name(g->led, "esh-led");	/* shows in gpioinfo */
	platform_set_drvdata(pdev, g);
	esh_gpio_selftest(dev, g);

	if (!g->button) {
		dev_info(dev, "LED only (no button-gpios)\n");
		return 0;
	}
	gpiod_set_consumer_name(g->button, "esh-button");

	irq = gpiod_to_irq(g->button);
	if (irq < 0)
		return dev_err_probe(dev, irq, "button line cannot interrupt\n");

	/*
	 * NULL primary handler + IRQF_ONESHOT: the line stays masked until
	 * the thread finished, so a level/edge storm cannot pile up.
	 * Both edges: we want press AND release (LED mirrors the button).
	 * devm_: freed on unbind BEFORE the gpio descriptors (reverse order).
	 */
	ret = devm_request_threaded_irq(dev, irq, NULL, esh_button_thread,
					IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
					IRQF_ONESHOT, DRV_NAME, g);
	if (ret)
		return dev_err_probe(dev, ret, "request irq %d\n", irq);

	dev_info(dev, "LED + button (irq %d, button %s, can_sleep=%d)\n", irq,
		 gpiod_get_value_cansleep(g->button) ? "pressed" : "released",
		 gpiod_cansleep(g->button));
	return 0;
}

static const struct of_device_id esh_gpio_of_match[] = {
	{ .compatible = "esh,gpio-demo" },
	{ }
};
MODULE_DEVICE_TABLE(of, esh_gpio_of_match);

static struct platform_driver esh_gpio_driver = {
	.probe = esh_gpio_probe,
	/* no .remove: everything is devm_ */
	.driver = {
		.name = DRV_NAME,
		.of_match_table = esh_gpio_of_match,
		.dev_groups = esh_gpio_groups,
	},
};

/* ---- sim_chip mode: what a board file did before device tree -------------- */

/*
 * Maps (device "esh-gpio-demo", function "led") -> (chip label, offset).
 * .key (chip label) and flags are filled in at init from the parameters.
 */
static struct gpiod_lookup_table esh_gpio_lookup = {
	.dev_id = DRV_NAME,
	.table = {
		GPIO_LOOKUP("", 0, "led", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("", 1, "button", GPIO_ACTIVE_LOW),
		{ }
	},
};

static struct platform_device *esh_sim_pdev;

static int __init esh_gpio_init(void)
{
	int ret;

	if (sim_chip && (led_line > 1023 || button_line > 1023 ||
			 led_line == button_line)) {
		pr_err("led_line=%u button_line=%u: must differ and be 0..1023\n",
		       led_line, button_line);
		return -EINVAL;
	}

	ret = platform_driver_register(&esh_gpio_driver);
	if (ret || !sim_chip)
		return ret;

	esh_gpio_lookup.table[0].key = sim_chip;
	esh_gpio_lookup.table[0].chip_hwnum = led_line;
	esh_gpio_lookup.table[0].flags = led_active_low ? GPIO_ACTIVE_LOW :
							  GPIO_ACTIVE_HIGH;
	esh_gpio_lookup.table[1].key = sim_chip;
	esh_gpio_lookup.table[1].chip_hwnum = button_line;
	gpiod_add_lookup_table(&esh_gpio_lookup);

	/* Name matches the driver -> probe runs (or defers) right here. */
	esh_sim_pdev = platform_device_register_simple(DRV_NAME,
						       PLATFORM_DEVID_NONE,
						       NULL, 0);
	if (IS_ERR(esh_sim_pdev)) {
		ret = PTR_ERR(esh_sim_pdev);
		gpiod_remove_lookup_table(&esh_gpio_lookup);
		platform_driver_unregister(&esh_gpio_driver);
		return ret;
	}
	pr_info("sim mode: chip \"%s\", led line %u%s, button line %u (active-low)\n",
		sim_chip, led_line, led_active_low ? " (active-low)" : "",
		button_line);
	return 0;
}
module_init(esh_gpio_init);

static void __exit esh_gpio_exit(void)
{
	/* reverse order of init: device, lookup table, driver */
	if (esh_sim_pdev) {
		platform_device_unregister(esh_sim_pdev);
		gpiod_remove_lookup_table(&esh_gpio_lookup);
	}
	platform_driver_unregister(&esh_gpio_driver);
}
module_exit(esh_gpio_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("embedded-systems-handbook");
MODULE_DESCRIPTION("Lesson 07: gpiod consumer driver (LED + button IRQ), DT or gpio-sim");
