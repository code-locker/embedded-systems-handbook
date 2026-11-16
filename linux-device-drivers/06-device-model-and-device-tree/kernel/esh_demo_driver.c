// SPDX-License-Identifier: GPL-2.0
/*
 * esh_demo_driver.c — a device-tree driver for the BeagleBone AI-64
 *                     ("esh,demo-device") plus a consumer that needs it and
 *                     DEFERS its probe until the provider is bound
 *
 * Concept : The shape of a real DT platform driver: an of_match_table with
 *           MODULE_DEVICE_TABLE(of, ...) so udev autoloads the module when the
 *           node appears, properties read with device_property_*(), child
 *           nodes, sysfs attributes, devm everywhere, a void .remove (6.11+).
 *           A second driver ("esh,demo-consumer") follows a phandle
 *           (esh,provider = <&esh_demo>) to the provider device; while that
 *           is not bound it returns -EPROBE_DEFER and the driver core retries
 *           it after the provider binds; then a device_link makes the core
 *           unbind the consumer first whenever the provider goes away.
 *           Board: the devices come from kernel/esh-demo-overlay.dtso.
 *           PC/VM: load with fake_devices=1 and the module creates the same
 *           two devices from software nodes (names and properties identical),
 *           and the drivers match them by NAME (platform_device_id).
 * Build   : make drivers  (overlay: make -C .../kernel dtbo)
 *           (or: make -C linux-device-drivers/06-device-model-and-device-tree/kernel)
 * Run     : board, overlay applied:  sudo insmod esh_demo_driver.ko
 *           VM (no DT):              sudo insmod esh_demo_driver.ko fake_devices=1
 *           dmesg | grep esh         -> deferred probe, PASS lines
 *           cat /sys/bus/platform/devices/esh-demo/label
 *           sudo rmmod esh_demo_driver
 * Read    : ../README.md  (sections 3.2 "Probe and deferred probe", 3.7
 *           "Reading properties", 3.8 "Overlays")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/sysfs.h>
#include <linux/version.h>

#define ESH_MAX_THR	8

/* platform_driver.remove returns void since 6.11; before, use .remove_new. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
#define ESH_REMOVE remove
#else
#define ESH_REMOVE remove_new
#endif

static bool fake_devices;
module_param(fake_devices, bool, 0444);
MODULE_PARM_DESC(fake_devices, "create esh-demo + esh-demo-consumer from software nodes (PC/VM without DT)");

static int nr_fail;
static atomic_t provider_probes = ATOMIC_INIT(0);
static atomic_t consumer_probes = ATOMIC_INIT(0);
static atomic_t consumer_defers = ATOMIC_INIT(0);

static __printf(3, 4) void check(bool ok, const char *test, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (ok) {
		pr_info("PASS %-10s %pV\n", test, &vaf);
	} else {
		nr_fail++;
		pr_err("FAIL %-10s %pV\n", test, &vaf);
	}
	va_end(args);
}

/* ======================================================================== */
/* Provider: "esh,demo-device"                                              */
/* ======================================================================== */
struct esh_demo {
	const char *label;
	u32 freq_hz;
	u32 thr[ESH_MAX_THR];
	int nr_thr;
	int nr_children;
};

static ssize_t label_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct esh_demo *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", d->label);
}
static DEVICE_ATTR_RO(label);

static ssize_t frequency_hz_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct esh_demo *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", d->freq_hz);
}
static DEVICE_ATTR_RO(frequency_hz);

static ssize_t thresholds_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct esh_demo *d = dev_get_drvdata(dev);
	int i, n = 0;

	for (i = 0; i < d->nr_thr; i++)
		n += sysfs_emit_at(buf, n, "%s%u", i ? " " : "", d->thr[i]);
	return n + sysfs_emit_at(buf, n, "\n");
}
static DEVICE_ATTR_RO(thresholds);

static struct attribute *esh_demo_attrs[] = {
	&dev_attr_label.attr,
	&dev_attr_frequency_hz.attr,
	&dev_attr_thresholds.attr,
	NULL,
};
ATTRIBUTE_GROUPS(esh_demo);

static int esh_demo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fwnode_handle *child;
	struct esh_demo *d;
	int err;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	/*
	 * device_property_*() works on DT (board), ACPI and software nodes
	 * (PC) alike. of_property_read_*(dev->of_node, ...) would be DT-only.
	 */
	err = device_property_read_string(dev, "label", &d->label);
	if (err)
		return dev_err_probe(dev, err, "required property 'label' missing\n");
	d->freq_hz = 1000;	/* binding default */
	device_property_read_u32(dev, "esh,frequency-hz", &d->freq_hz);
	d->nr_thr = device_property_count_u32(dev, "esh,thresholds");
	if (d->nr_thr == -EINVAL)
		d->nr_thr = 0;	/* property absent: optional */
	if (d->nr_thr < 0 || d->nr_thr > ESH_MAX_THR)
		return dev_err_probe(dev, -EINVAL, "esh,thresholds: up to %d cells\n", ESH_MAX_THR);
	if (d->nr_thr) {
		err = device_property_read_u32_array(dev, "esh,thresholds", d->thr, d->nr_thr);
		if (err)
			return dev_err_probe(dev, err, "esh,thresholds\n");
	}
	device_for_each_child_node(dev, child) {
		const char *lbl = "?";
		u32 reg = 0;

		fwnode_property_read_u32(child, "reg", &reg);
		fwnode_property_read_string(child, "label", &lbl);
		dev_info(dev, "  channel reg=%u label=%s\n", reg, lbl);
		d->nr_children++;
	}

	platform_set_drvdata(pdev, d);	/* the consumer reads this: set it last */
	atomic_inc(&provider_probes);
	dev_info(dev, "provider probed (%s): label=%s frequency=%u Hz thresholds=%d channels=%d\n",
		 dev_of_node(dev) ? "device tree" : "software node", d->label, d->freq_hz,
		 d->nr_thr, d->nr_children);
	return 0;
}

static void esh_demo_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "provider removed\n");
}

static const struct of_device_id esh_demo_of_match[] = {
	{ .compatible = "esh,demo-device" },
	{ }
};
MODULE_DEVICE_TABLE(of, esh_demo_of_match);	/* alias "of:N*T*Cesh,demo-device" */

static const struct platform_device_id esh_demo_ids[] = {
	{ .name = "esh-demo" },			/* PC: matched by device name */
	{ }
};
MODULE_DEVICE_TABLE(platform, esh_demo_ids);

static struct platform_driver esh_demo_driver = {
	.probe		= esh_demo_probe,
	.ESH_REMOVE	= esh_demo_remove,
	.id_table	= esh_demo_ids,
	.driver = {
		.name		= "esh-demo",
		.of_match_table	= esh_demo_of_match,
		.dev_groups	= esh_demo_groups,
	},
};

/* ======================================================================== */
/* Consumer: "esh,demo-consumer" with esh,provider = <&esh_demo>            */
/* ======================================================================== */
struct esh_consumer {
	struct esh_demo *prov;
	struct device *prov_dev;
};

static ssize_t provider_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct esh_consumer *c = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s (%s)\n", dev_name(c->prov_dev), c->prov->label);
}
static DEVICE_ATTR_RO(provider);

static struct attribute *esh_consumer_attrs[] = {
	&dev_attr_provider.attr,
	NULL,
};
ATTRIBUTE_GROUPS(esh_consumer);

/*
 *   consumer probe()                                  driver core
 *   ----------------                                  -----------
 *   follow esh,provider phandle -> fwnode
 *   find the platform device with that fwnode
 *   not there / no driver bound?  -> -EPROBE_DEFER --> park on the deferred list
 *                                                      (/sys/kernel/debug/devices_deferred)
 *                                   provider binds --> retry every deferred device
 *   bound: device_link_add(consumer -> supplier)      unbinding the supplier now
 *          use its drvdata                            unbinds us first
 */
static int esh_consumer_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fwnode_handle *ref;
	struct esh_consumer *c;
	struct device_link *link;
	struct device *sup;
	struct esh_demo *prov;

	ref = fwnode_find_reference(dev_fwnode(dev), "esh,provider", 0);
	if (IS_ERR(ref))
		return dev_err_probe(dev, PTR_ERR(ref), "no esh,provider phandle\n");
	sup = bus_find_device_by_fwnode(&platform_bus_type, ref);	/* takes a ref */
	fwnode_handle_put(ref);

	prov = sup ? dev_get_drvdata(sup) : NULL;
	if (!sup || !READ_ONCE(sup->driver) || !prov) {
		atomic_inc(&consumer_defers);
		dev_info(dev, "provider %s -> -EPROBE_DEFER\n",
			 sup ? "not bound yet" : "device does not exist yet");
		put_device(sup);	/* NULL-safe */
		/* quiet for -EPROBE_DEFER; the reason shows in devices_deferred */
		return dev_err_probe(dev, -EPROBE_DEFER, "waiting for esh,provider\n");
	}

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c) {
		put_device(sup);
		return -ENOMEM;
	}
	/*
	 * A managed link: the core will unbind us before it unbinds the
	 * supplier, so c->prov never dangles. AUTOREMOVE_CONSUMER deletes the
	 * link when we unbind. The link holds its own device references.
	 * (For standard bindings — clocks, gpios, regulators... — fw_devlink
	 * creates such links from the DT automatically.)
	 */
	link = device_link_add(dev, sup, DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(sup);
	if (!link)
		return dev_err_probe(dev, -EINVAL, "device_link_add failed\n");

	c->prov = prov;
	c->prov_dev = sup;
	platform_set_drvdata(pdev, c);
	atomic_inc(&consumer_probes);
	dev_info(dev, "consumer probed after %d deferral(s): provider %s label=%s\n",
		 atomic_read(&consumer_defers), dev_name(sup), prov->label);
	return 0;
}

static void esh_consumer_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "consumer removed\n");
}

static const struct of_device_id esh_consumer_of_match[] = {
	{ .compatible = "esh,demo-consumer" },
	{ }
};
MODULE_DEVICE_TABLE(of, esh_consumer_of_match);

static const struct platform_device_id esh_consumer_ids[] = {
	{ .name = "esh-demo-consumer" },
	{ }
};
MODULE_DEVICE_TABLE(platform, esh_consumer_ids);

static struct platform_driver esh_consumer_driver = {
	.probe		= esh_consumer_probe,
	.ESH_REMOVE	= esh_consumer_remove,
	.id_table	= esh_consumer_ids,
	.driver = {
		.name		= "esh-demo-consumer",
		.of_match_table	= esh_consumer_of_match,
		.dev_groups	= esh_consumer_groups,
	},
};

/* ======================================================================== */
/* fake_devices=1: the same two nodes as esh-demo-overlay.dtso, in software */
/* ======================================================================== */
static const u32 fake_thr[] = { 100, 200, 300 };

static const struct property_entry fake_prov_props[] = {
	PROPERTY_ENTRY_STRING("label", "fake-demo"),
	PROPERTY_ENTRY_U32("esh,frequency-hz", 1000000),
	PROPERTY_ENTRY_U32_ARRAY("esh,thresholds", fake_thr),
	{ }
};

static const struct software_node fake_prov_node = {
	.name = "esh-demo",
	.properties = fake_prov_props,
};

static const struct property_entry fake_ch0_props[] = {
	PROPERTY_ENTRY_U32("reg", 0),
	PROPERTY_ENTRY_STRING("label", "adc0"),
	{ }
};

static const struct software_node fake_ch0_node = {
	.name = "channel@0",
	.parent = &fake_prov_node,
	.properties = fake_ch0_props,
};

static const struct property_entry fake_ch1_props[] = {
	PROPERTY_ENTRY_U32("reg", 1),
	PROPERTY_ENTRY_STRING("label", "adc1"),
	{ }
};

static const struct software_node fake_ch1_node = {
	.name = "channel@1",
	.parent = &fake_prov_node,
	.properties = fake_ch1_props,
};

static const struct property_entry fake_cons_props[] = {
	PROPERTY_ENTRY_REF("esh,provider", &fake_prov_node),	/* = <&esh_demo> */
	{ }
};

static const struct software_node fake_cons_node = {
	.name = "esh-demo-consumer",
	.properties = fake_cons_props,
};

static const struct software_node *fake_nodes[] = {
	&fake_prov_node, &fake_ch0_node, &fake_ch1_node, &fake_cons_node, NULL
};

static struct platform_device *fake_prov, *fake_cons;

static struct platform_device *fake_register(const char *name, const struct software_node *node)
{
	struct platform_device_info info = {
		.name	= name,
		.id	= PLATFORM_DEVID_NONE,		/* dev name = "esh-demo", like DT */
		.fwnode	= software_node_fwnode(node),
	};

	return platform_device_register_full(&info);
}

static void fake_unregister(void)
{
	if (!IS_ERR_OR_NULL(fake_cons))
		platform_device_unregister(fake_cons);
	if (!IS_ERR_OR_NULL(fake_prov))
		platform_device_unregister(fake_prov);
	fake_cons = NULL;
	fake_prov = NULL;
	software_node_unregister_node_group(fake_nodes);
}

static int fake_register_all(void)
{
	int err = software_node_register_node_group(fake_nodes);

	if (err)
		return err;
	/* consumer device first: its driver will find no provider */
	fake_cons = fake_register("esh-demo-consumer", &fake_cons_node);
	if (IS_ERR(fake_cons)) {
		err = PTR_ERR(fake_cons);
		goto fail;
	}
	fake_prov = fake_register("esh-demo", &fake_prov_node);
	if (IS_ERR(fake_prov)) {
		err = PTR_ERR(fake_prov);
		goto fail;
	}
	return 0;
fail:
	fake_unregister();
	return err;
}

/* ======================================================================== */

static int __init esh_demo_init(void)
{
	int err;

	if (fake_devices) {
		err = fake_register_all();	/* devices exist before drivers, like DT */
		if (err)
			return err;
	}

	/*
	 * Consumer driver FIRST, on purpose: it binds (tries to) before the
	 * provider driver exists, so we see -EPROBE_DEFER. In real life the
	 * order is whatever module loading and probing produce — which is
	 * exactly why deferred probe exists.
	 */
	err = platform_driver_register(&esh_consumer_driver);
	if (err)
		goto err_fake;
	err = platform_driver_register(&esh_demo_driver);
	if (err)
		goto err_consumer;

	/* the provider's successful bind queued a retry of deferred devices */
	wait_for_device_probe();

	if (!atomic_read(&provider_probes) && !atomic_read(&consumer_defers)) {
		pr_info("INFO no esh,demo-* devices: apply esh-demo-overlay.dtbo (board) or load with fake_devices=1 (PC/VM)\n");
		return 0;
	}
	check(atomic_read(&provider_probes) == 1, "provider", "bound %d time(s)",
	      atomic_read(&provider_probes));
	if (atomic_read(&consumer_defers) || atomic_read(&consumer_probes))
		check(atomic_read(&consumer_defers) >= 1 && atomic_read(&consumer_probes) == 1,
		      "deferred", "consumer deferred %d time(s), then bound %d time(s)",
		      atomic_read(&consumer_defers), atomic_read(&consumer_probes));
	else
		pr_info("INFO no esh,demo-consumer device: deferred probe not exercised\n");
	if (nr_fail)
		pr_err("done: %d check(s) FAILED\n", nr_fail);
	else
		pr_info("done: all checks passed\n");
	/* Return 0 even on FAIL so the module can be inspected and rmmod'ed. */
	return 0;

err_consumer:
	platform_driver_unregister(&esh_consumer_driver);
err_fake:
	if (fake_devices)
		fake_unregister();
	return err;
}

static void __exit esh_demo_exit(void)
{
	/*
	 * Provider driver first: the device link makes the core unbind the
	 * consumer before the provider ("consumer removed" logs first).
	 */
	platform_driver_unregister(&esh_demo_driver);
	platform_driver_unregister(&esh_consumer_driver);
	if (fake_devices)
		fake_unregister();
	pr_info("unloaded\n");
}

module_init(esh_demo_init);
module_exit(esh_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("DT platform driver esh,demo-device + consumer with deferred probe and device link");
