// SPDX-License-Identifier: GPL-2.0
/*
 * gpio_button_irq.c — a real GPIO interrupt on the BeagleBone AI-64: push
 *                     button on header pin P8_16, both edges, debounced
 *
 * Concept : The complete shape of a small interrupt-driven platform driver:
 *           matched by the DT overlay node (compatible "esh,gpio-button"),
 *           it gets its GPIO with devm_gpiod_get(), turns it into an IRQ
 *           number with gpiod_to_irq() and requests a THREADED, ONESHOT,
 *           both-edges IRQ with devm_request_threaded_irq(). The hard
 *           handler only counts raw edges (contact bounce gives many); the
 *           thread (re)starts a debounce delayed_work; the work reads the
 *           settled level and counts presses/releases. Counters in sysfs.
 *           Everything is devm-managed, so remove() is empty and the
 *           release order is the reverse of the order things were acquired.
 * Build   : make drivers
 *           (or: make -C linux-device-drivers/04-interrupts-timers-and-deferred-work/kernel)
 *           overlay: make -C .../kernel dtbo KDIR=<board kernel tree>
 * Run     : BOARD ONLY (needs gpio-button-overlay.dtbo applied, see README):
 *           sudo insmod gpio_button_irq.ko ; press the button
 *           dmesg | grep esh-button ; cat /sys/bus/platform/devices/esh-button/presses
 *           sudo rmmod gpio_button_irq
 *           On a PC it loads but binds to nothing (no such DT node).
 * Read    : ../README.md  (section 3.7 "A real GPIO interrupt on the board")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define BTN_DEBOUNCE_DEFAULT_MS	20
#define BTN_DEBOUNCE_MAX_MS	1000

struct btn {
	struct device *dev;
	struct gpio_desc *gpiod;
	int irq;
	u32 debounce_ms;
	atomic_t raw_edges;		/* hard handler: every edge, bounces included */
	atomic64_t last_edge_ns;
	struct delayed_work debounce;
	int stable;			/* last debounced level (only the work writes) */
	unsigned int presses, releases;	/* only the work writes; sysfs reads */
};

/*
 *   pin level   ~~~~~|_|~|_|~|______________|~|_|~~~~~~~~~~   (active-low button)
 *   raw edges        ^ ^ ^ ^ ^              ^ ^ ^             hard handler counts all
 *   debounce work    re-armed on each edge [20 ms quiet] -> read 1 = PRESS
 *                                                     ... [20 ms quiet] -> read 0 = RELEASE
 */

/* TOP HALF: hard-IRQ context (an irq thread on PREEMPT_RT). Count, stamp, defer. */
static irqreturn_t btn_hard(int irq, void *dev_id)
{
	struct btn *b = dev_id;

	atomic_inc(&b->raw_edges);
	atomic64_set(&b->last_edge_ns, ktime_get_ns());
	return IRQ_WAKE_THREAD;
}

/*
 * THREADED HANDLER: process context. Restart the debounce timer: only when
 * the pin has been quiet for debounce_ms does the work item run. Here a
 * thread is not strictly needed (mod_delayed_work() is IRQ-safe), but it is
 * the right place for anything that may sleep — e.g. reading a GPIO that
 * sits on an I2C expander (gpiod_cansleep() == true).
 */
static irqreturn_t btn_thread(int irq, void *dev_id)
{
	struct btn *b = dev_id;

	mod_delayed_work(system_wq, &b->debounce,
			 msecs_to_jiffies(READ_ONCE(b->debounce_ms)));
	return IRQ_HANDLED;
}

/* Runs once the contact has settled. Logical value: 1 = pressed (ACTIVE_LOW handled by gpiolib). */
static void btn_debounce_fn(struct work_struct *w)
{
	struct btn *b = container_of(to_delayed_work(w), struct btn, debounce);
	int v = gpiod_get_value_cansleep(b->gpiod);

	if (v < 0 || v == b->stable)
		return;		/* read error, or a bounce that came back to the old state */
	b->stable = v;
	if (v)
		WRITE_ONCE(b->presses, b->presses + 1);
	else
		WRITE_ONCE(b->releases, b->releases + 1);
	dev_info(b->dev, "%s (presses %u, releases %u, raw edges %d)\n",
		 v ? "pressed" : "released", b->presses, b->releases,
		 atomic_read(&b->raw_edges));
	sysfs_notify(&b->dev->kobj, NULL, v ? "presses" : "releases");	/* wakes poll() */
}

/*
 * sysfs: /sys/bus/platform/devices/esh-button/
 *        presses releases raw_edges state debounce_ms
 */
static ssize_t presses_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct btn *b = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(b->presses));
}
static DEVICE_ATTR_RO(presses);

static ssize_t releases_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct btn *b = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(b->releases));
}
static DEVICE_ATTR_RO(releases);

static ssize_t raw_edges_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct btn *b = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", atomic_read(&b->raw_edges));
}
static DEVICE_ATTR_RO(raw_edges);

static ssize_t state_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct btn *b = dev_get_drvdata(dev);

	/* sysfs show runs in process context: the sleeping accessor is fine */
	return sysfs_emit(buf, "%d\n", gpiod_get_value_cansleep(b->gpiod));
}
static DEVICE_ATTR_RO(state);

static ssize_t debounce_ms_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct btn *b = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(b->debounce_ms));
}

static ssize_t debounce_ms_store(struct device *dev, struct device_attribute *a,
				 const char *buf, size_t len)
{
	struct btn *b = dev_get_drvdata(dev);
	u32 v;
	int err = kstrtou32(buf, 0, &v);

	if (err)
		return err;
	if (v < 1 || v > BTN_DEBOUNCE_MAX_MS)
		return -ERANGE;
	WRITE_ONCE(b->debounce_ms, v);
	return len;
}
static DEVICE_ATTR_RW(debounce_ms);

static struct attribute *btn_attrs[] = {
	&dev_attr_presses.attr,
	&dev_attr_releases.attr,
	&dev_attr_raw_edges.attr,
	&dev_attr_state.attr,
	&dev_attr_debounce_ms.attr,
	NULL,
};
ATTRIBUTE_GROUPS(btn);

/* devm action: runs at unbind AFTER the IRQ is freed (devm releases in reverse). */
static void btn_cancel_work(void *data)
{
	struct btn *b = data;

	cancel_delayed_work_sync(&b->debounce);
}

static int btn_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct btn *b;
	int err;

	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);	/* freed automatically on unbind */
	if (!b)
		return -ENOMEM;
	b->dev = dev;
	atomic_set(&b->raw_edges, 0);
	INIT_DELAYED_WORK(&b->debounce, btn_debounce_fn);
	platform_set_drvdata(pdev, b);

	b->debounce_ms = BTN_DEBOUNCE_DEFAULT_MS;
	device_property_read_u32(dev, "debounce-ms", &b->debounce_ms);	/* optional */
	if (b->debounce_ms < 1 || b->debounce_ms > BTN_DEBOUNCE_MAX_MS)
		return dev_err_probe(dev, -EINVAL, "debounce-ms must be 1..%d\n",
				     BTN_DEBOUNCE_MAX_MS);

	/*
	 * "button-gpios" in DT -> con_id "button". May return -EPROBE_DEFER if
	 * the GPIO controller has not probed yet: dev_err_probe() stays quiet
	 * for that case and the driver core retries us later.
	 */
	b->gpiod = devm_gpiod_get(dev, "button", GPIOD_IN);
	if (IS_ERR(b->gpiod))
		return dev_err_probe(dev, PTR_ERR(b->gpiod), "no button-gpios\n");

	/* Hardware debounce if the GPIO controller has it (gpio-davinci: no). */
	err = gpiod_set_debounce(b->gpiod, b->debounce_ms * USEC_PER_MSEC);
	dev_info(dev, "hardware debounce %s, using %u ms software debounce\n",
		 err ? "not supported" : "enabled", b->debounce_ms);

	b->irq = gpiod_to_irq(b->gpiod);
	if (b->irq < 0)
		return dev_err_probe(dev, b->irq, "GPIO cannot interrupt\n");

	b->stable = gpiod_get_value_cansleep(b->gpiod);

	/* Register the work cancel BEFORE the IRQ: devm undoes in reverse order. */
	err = devm_add_action_or_reset(dev, btn_cancel_work, b);
	if (err)
		return err;

	/*
	 * Both edges, so we see press and release. IRQF_ONESHOT keeps the line
	 * masked until btn_thread() returns. The trigger flags override the
	 * type from DT for this request. devm_: free_irq() runs on unbind.
	 */
	err = devm_request_threaded_irq(dev, b->irq, btn_hard, btn_thread,
					IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					dev_name(dev), b);
	if (err)
		return dev_err_probe(dev, err, "request irq %d\n", b->irq);

	dev_info(dev, "ready: irq %d, level now %d (1 = pressed)\n", b->irq, b->stable);
	return 0;
}

static const struct of_device_id btn_of_match[] = {
	{ .compatible = "esh,gpio-button" },
	{ }
};
MODULE_DEVICE_TABLE(of, btn_of_match);	/* modalias -> udev autoloads us when the node appears */

static struct platform_driver btn_driver = {
	.probe = btn_probe,
	/* no .remove: every resource is devm-managed */
	.driver = {
		.name		= "esh-gpio-button",
		.of_match_table	= btn_of_match,
		.dev_groups	= btn_groups,	/* created before probe returns to user space */
	},
};
module_platform_driver(btn_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("GPIO push-button IRQ demo: threaded IRQ, debounce, sysfs counters");
