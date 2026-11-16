// SPDX-License-Identifier: GPL-2.0
/*
 * platdev_host_demo.c — the Linux device model on a PC without a device tree:
 *                       a platform_device + platform_driver pair, software-node
 *                       properties, resources, MMIO-style registers, devm, sysfs
 *
 * Concept : On ARM the device tree describes the board and the kernel creates
 *           platform devices from it. x86 has no DT, so this module plays
 *           both roles: like an old "board file" it registers a software
 *           node tree (properties + two child nodes, PROPERTY_ENTRY_*) and a
 *           platform_device with a MEM resource; then its platform_driver is
 *           matched BY NAME and probes. The driver reads everything through
 *           device_property_*() / fwnode — the SAME calls that read a DT node
 *           on the board. It programs a register map with readl/writel +
 *           FIELD_PREP, allocates with devm_*, exposes sysfs attributes, and
 *           proves at load time that unbind releases devm resources in
 *           reverse order and that rebinding probes again.
 * Build   : make drivers
 *           (or: make -C linux-device-drivers/06-device-model-and-device-tree/kernel)
 * Run     : sudo insmod platdev_host_demo.ko
 *           dmesg | grep platdev            -> PASS lines
 *           ls -l /sys/bus/platform/devices/platdev-demo.0/
 *                 (driver, software_node, label, ctrl_mode)
 *           sudo rmmod platdev_host_demo
 *           (in a VM or on the board — never on a machine you care about)
 * Read    : ../README.md  (sections 3.1 "Bus, device, driver", 3.3 "devm",
 *           3.4 "Platform bus, resources and MMIO", 3.7 "Reading properties")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/version.h>

#define DRV_NAME	"platdev-demo"
#define DEMO_MAX_THR	8

static int nr_fail;

static __printf(3, 4) void check(bool ok, const char *test, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (ok) {
		pr_info("PASS %-11s %pV\n", test, &vaf);
	} else {
		nr_fail++;
		pr_err("FAIL %-11s %pV\n", test, &vaf);
	}
	va_end(args);
}

/* ======================================================================== */
/* PART 1 — the "board": software nodes + a platform_device                 */
/* ======================================================================== */
/*
 * The software node tree below is the moral equivalent of this DT fragment:
 *
 *   platdev-demo {
 *           label = "host-demo";
 *           esh,frequency-hz = <1000000>;
 *           esh,thresholds = <100 200 300>;
 *           esh,simulated-regs;                 // bool: "no real MMIO here"
 *           channel@0 { reg = <0>; label = "adc0"; };
 *           channel@1 { reg = <1>; label = "adc1"; };
 *   };
 */
static const u32 demo_thresholds[] = { 100, 200, 300 };

static const struct property_entry demo_props[] = {
	PROPERTY_ENTRY_STRING("label", "host-demo"),
	PROPERTY_ENTRY_U32("esh,frequency-hz", 1000000),
	PROPERTY_ENTRY_U32_ARRAY("esh,thresholds", demo_thresholds),
	PROPERTY_ENTRY_BOOL("esh,simulated-regs"),
	{ }
};

static const struct property_entry ch0_props[] = {
	PROPERTY_ENTRY_U32("reg", 0),
	PROPERTY_ENTRY_STRING("label", "adc0"),
	{ }
};

static const struct property_entry ch1_props[] = {
	PROPERTY_ENTRY_U32("reg", 1),
	PROPERTY_ENTRY_STRING("label", "adc1"),
	{ }
};

static const struct software_node demo_node = {
	.name = "platdev-demo",
	.properties = demo_props,
};

static const struct software_node ch0_node = {
	.name = "channel@0",
	.parent = &demo_node,
	.properties = ch0_props,
};

static const struct software_node ch1_node = {
	.name = "channel@1",
	.parent = &demo_node,
	.properties = ch1_props,
};

/* Registered parents first; unregistered in reverse by the group helpers. */
static const struct software_node *demo_nodes[] = { &demo_node, &ch0_node, &ch1_node, NULL };

/*
 * A pretend bus with one 256-byte register window at 0x4000_1000. The
 * resource is inserted under THIS private root, not under the global
 * iomem_resource (/proc/iomem), because the address is fake: inserting it
 * there could collide with real RAM or devices of the PC.
 */
static struct resource fake_bus = DEFINE_RES_MEM_NAMED(0x40000000, 0x10000, "esh-fake-bus");

static struct platform_device *demo_pdev;

static int demo_board_register(void)
{
	struct resource res = DEFINE_RES_MEM_NAMED(0x40001000, 0x100, "regs");
	int err;

	res.parent = &fake_bus;		/* see above; copied by add_resources */

	err = software_node_register_node_group(demo_nodes);
	if (err)
		return err;

	/*
	 * The long way, to show what platform_device_register_full() does:
	 * alloc -> add resources -> attach firmware node -> add (= register).
	 * After platform_device_alloc() the object is refcounted: free it with
	 * platform_device_put(), never kfree().
	 */
	demo_pdev = platform_device_alloc(DRV_NAME, 0);		/* -> "platdev-demo.0" */
	if (!demo_pdev) {
		err = -ENOMEM;
		goto err_nodes;
	}
	err = platform_device_add_resources(demo_pdev, &res, 1);	/* copies res */
	if (err)
		goto err_put;
	err = device_add_software_node(&demo_pdev->dev, &demo_node);
	if (err)
		goto err_put;
	err = platform_device_add(demo_pdev);	/* -> bus matching -> maybe probe() */
	if (err)
		goto err_swnode;
	return 0;

err_swnode:
	device_remove_software_node(&demo_pdev->dev);
err_put:
	platform_device_put(demo_pdev);
	demo_pdev = NULL;
err_nodes:
	software_node_unregister_node_group(demo_nodes);
	return err;
}

static void demo_board_unregister(void)
{
	platform_device_del(demo_pdev);			/* unbinds the driver -> remove() */
	device_remove_software_node(&demo_pdev->dev);
	platform_device_put(demo_pdev);			/* last ref -> release -> freed */
	demo_pdev = NULL;
	software_node_unregister_node_group(demo_nodes);
}

/* ======================================================================== */
/* PART 2 — the driver                                                      */
/* ======================================================================== */

/* Register map of our (pretend) IP block — the same idea as a TRM chapter. */
#define DEMO_REG_ID		0x00	/* RO: IP id + revision */
#define DEMO_REG_CTRL		0x04	/* RW */
#define   CTRL_EN		BIT(0)
#define   CTRL_MODE		GENMASK(7, 4)
#define   CTRL_DIV		GENMASK(31, 16)
#define DEMO_REG_STATUS		0x08	/* RO */
#define   STATUS_READY		BIT(0)
#define DEMO_ID_VALUE		0xe5d00001
#define DEMO_REF_CLK_HZ		48000000	/* the "functional clock" (TDA4VM UART fclk) */

struct demo_priv {
	struct device *dev;
	void __iomem *regs;
	const char *label;
	u32 freq_hz;
	u32 thr[DEMO_MAX_THR];
	int nr_thr;
	int nr_children;
	u32 child_reg_sum;
	resource_size_t res_start, res_size;
	int irq;
};

/* What the self-test observes. Written by probe/remove/devm actions. */
static struct {
	int probes, removes;
	char release_order[8];	/* devm actions append 'A','B','C' when run */
	int nr_released;
	struct demo_priv *priv;	/* last probed instance (for the checks) */
} seen;

static void devm_action_log(void *data)
{
	const char *tag = data;

	if (seen.nr_released < (int)sizeof(seen.release_order) - 1)
		seen.release_order[seen.nr_released++] = *tag;
	pr_info("  devm release %c\n", *tag);
}

/* Fake hardware: give the "RAM registers" their reset values. */
static void demo_fake_hw_reset(struct demo_priv *p)
{
	writel(DEMO_ID_VALUE, p->regs + DEMO_REG_ID);
	writel(0, p->regs + DEMO_REG_CTRL);
	writel(STATUS_READY, p->regs + DEMO_REG_STATUS);
}

static ssize_t label_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct demo_priv *p = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", p->label);
}
static DEVICE_ATTR_RO(label);

/* ctrl_mode: read-modify-write of the MODE field of CTRL */
static ssize_t ctrl_mode_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct demo_priv *p = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lu\n", FIELD_GET(CTRL_MODE, readl(p->regs + DEMO_REG_CTRL)));
}

static ssize_t ctrl_mode_store(struct device *dev, struct device_attribute *a,
			       const char *buf, size_t len)
{
	struct demo_priv *p = dev_get_drvdata(dev);
	u32 v, ctrl;
	int err = kstrtou32(buf, 0, &v);

	if (err)
		return err;
	if (v > FIELD_MAX(CTRL_MODE))
		return -ERANGE;
	/*
	 * Read-modify-write. Two writers at once could lose an update; the
	 * sysfs core serialises nothing for us. A real driver takes a lock
	 * here (03-kernel-synchronization). Kept lock-free for brevity.
	 */
	ctrl = readl(p->regs + DEMO_REG_CTRL);
	ctrl &= ~CTRL_MODE;
	ctrl |= FIELD_PREP(CTRL_MODE, v);
	writel(ctrl, p->regs + DEMO_REG_CTRL);
	return len;
}
static DEVICE_ATTR_RW(ctrl_mode);

static struct attribute *demo_attrs[] = {
	&dev_attr_label.attr,
	&dev_attr_ctrl_mode.attr,
	NULL,
};
ATTRIBUTE_GROUPS(demo);

static int demo_read_children(struct demo_priv *p)
{
	struct fwnode_handle *child;

	/*
	 * Same loop on DT (child nodes) and on software nodes. Leaving the
	 * loop early would need fwnode_handle_put(child) — the iterator holds
	 * a reference (6.12+: device_for_each_child_node_scoped() drops it).
	 */
	device_for_each_child_node(p->dev, child) {
		const char *lbl = "?";
		u32 reg = 0;

		fwnode_property_read_u32(child, "reg", &reg);
		fwnode_property_read_string(child, "label", &lbl);
		dev_info(p->dev, "  child %pfwP: reg=%u label=%s\n", child, reg, lbl);
		p->child_reg_sum += reg;
		p->nr_children++;
	}
	return p->nr_children;
}

static int demo_probe(struct platform_device *pdev)
{
	static const char tags[] = "ABC";	/* action data: one char each */
	struct device *dev = &pdev->dev;
	struct demo_priv *p;
	struct resource *res;
	u32 id, ctrl, div;
	int err;

	dev_info(dev, "probe: matched by %s, fwnode %pfwP\n",
		 platform_get_device_id(pdev) ? "platform_device_id" : "name", dev_fwnode(dev));

	/* devm_kzalloc: freed automatically after remove() (or a failed probe) */
	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	platform_set_drvdata(pdev, p);

	/* three no-op cleanup actions, to watch the release ORDER at unbind */
	err = devm_add_action_or_reset(dev, devm_action_log, (void *)&tags[0]);
	if (!err)
		err = devm_add_action_or_reset(dev, devm_action_log, (void *)&tags[1]);
	if (!err)
		err = devm_add_action_or_reset(dev, devm_action_log, (void *)&tags[2]);
	if (err)
		return err;

	/* ---- properties: identical calls for DT, ACPI and software nodes ---- */
	err = device_property_read_string(dev, "label", &p->label);
	if (err)
		return dev_err_probe(dev, err, "missing required property 'label'\n");
	p->freq_hz = 1000;				/* default if absent */
	device_property_read_u32(dev, "esh,frequency-hz", &p->freq_hz);
	if (p->freq_hz == 0 || p->freq_hz > DEMO_REF_CLK_HZ)
		return dev_err_probe(dev, -EINVAL, "esh,frequency-hz out of range\n");
	p->nr_thr = device_property_count_u32(dev, "esh,thresholds");
	if (p->nr_thr < 0 || p->nr_thr > DEMO_MAX_THR)
		return dev_err_probe(dev, -EINVAL, "esh,thresholds: 0..%d cells\n", DEMO_MAX_THR);
	if (p->nr_thr) {
		err = device_property_read_u32_array(dev, "esh,thresholds", p->thr, p->nr_thr);
		if (err)
			return dev_err_probe(dev, err, "esh,thresholds\n");
	}
	demo_read_children(p);

	/* ---- resources ---- */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return dev_err_probe(dev, -ENODEV, "no MEM resource\n");
	p->res_start = res->start;
	p->res_size = resource_size(res);
	/* optional IRQ: -ENXIO when there is none, without an error message */
	p->irq = platform_get_irq_optional(pdev, 0);

	/*
	 * ---- registers ----
	 * Real hardware:  p->regs = devm_platform_ioremap_resource(pdev, 0);
	 *   = request_mem_region (exclusive ownership, shows in /proc/iomem)
	 *     + ioremap (uncached device mapping) + both undone at unbind.
	 * Here the address is fake, so the "registers" are 256 bytes of RAM.
	 * readl/writel on RAM behave like on MMIO on x86 and arm64; NEVER do
	 * this in a real driver.
	 */
	if (device_property_read_bool(dev, "esh,simulated-regs")) {
		void *ram = devm_kzalloc(dev, p->res_size, GFP_KERNEL);

		if (!ram)
			return -ENOMEM;
		p->regs = (__force void __iomem *)ram;
		demo_fake_hw_reset(p);
	} else {
		p->regs = devm_platform_ioremap_resource(pdev, 0);
		if (IS_ERR(p->regs))
			return PTR_ERR(p->regs);
	}

	id = readl(p->regs + DEMO_REG_ID);
	if (id != DEMO_ID_VALUE)
		return dev_err_probe(dev, -ENODEV, "unexpected ID 0x%08x\n", id);
	if (!(readl(p->regs + DEMO_REG_STATUS) & STATUS_READY))
		return dev_err_probe(dev, -EPROBE_DEFER, "IP not ready yet\n");

	/* program: divider for the requested frequency, mode 3, enable */
	div = DIV_ROUND_CLOSEST(DEMO_REF_CLK_HZ, p->freq_hz);
	if (div > FIELD_MAX(CTRL_DIV))
		return dev_err_probe(dev, -ERANGE, "divider %u too large\n", div);
	ctrl = FIELD_PREP(CTRL_DIV, div) | FIELD_PREP(CTRL_MODE, 3) | CTRL_EN;
	writel(ctrl, p->regs + DEMO_REG_CTRL);	/* writel = barrier + store */

	seen.priv = p;
	seen.probes++;
	dev_info(dev, "probed: label=%s freq=%u Hz div=%u thresholds=%d children=%d regs@%pa\n",
		 p->label, p->freq_hz, div, p->nr_thr, p->nr_children, &p->res_start);
	return 0;
}

/* .remove became void-returning in 6.11 (via .remove_new since 6.3). */
static void demo_remove(struct platform_device *pdev)
{
	struct demo_priv *p = platform_get_drvdata(pdev);

	/* stop the "hardware" before devm frees what it might still use */
	writel(0, p->regs + DEMO_REG_CTRL);
	seen.removes++;
	dev_info(&pdev->dev, "remove: device disabled; devm releases follow\n");
}

static const struct platform_device_id demo_ids[] = {
	{ .name = DRV_NAME },
	{ }
};
MODULE_DEVICE_TABLE(platform, demo_ids);	/* -> alias "platform:platdev-demo" */

static struct platform_driver demo_driver = {
	.probe		= demo_probe,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	.remove		= demo_remove,
#else
	.remove_new	= demo_remove,
#endif
	.id_table	= demo_ids,
	.driver = {
		.name		= DRV_NAME,
		.dev_groups	= demo_groups,
	},
};

/* ======================================================================== */
/* PART 3 — self-test                                                       */
/* ======================================================================== */
static void self_test(void)
{
	struct demo_priv *p = seen.priv;
	struct device *dev = &demo_pdev->dev;
	u32 ctrl;
	int ret;

	check(seen.probes == 1 && dev->driver == &demo_driver.driver && p, "probe",
	      "%s bound to driver %s", dev_name(dev), dev->driver ? dev->driver->name : "(none)");
	if (!p)
		return;

	check(!strcmp(p->label, "host-demo") && p->freq_hz == 1000000 && p->nr_thr == 3 &&
	      p->thr[0] == 100 && p->thr[1] == 200 && p->thr[2] == 300, "properties",
	      "label=%s esh,frequency-hz=%u esh,thresholds=<%u %u %u> via device_property_*()",
	      p->label, p->freq_hz, p->thr[0], p->thr[1], p->thr[2]);
	check(p->nr_children == 2 && p->child_reg_sum == 1, "children",
	      "%d child nodes found with device_for_each_child_node()", p->nr_children);
	check(p->res_start == 0x40001000 && p->res_size == 0x100 && p->irq == -ENXIO,
	      "resources", "MEM %pa size %pa, platform_get_irq_optional() = %d (no IRQ)",
	      &p->res_start, &p->res_size, p->irq);

	ctrl = readl(p->regs + DEMO_REG_CTRL);
	check(FIELD_GET(CTRL_EN, ctrl) == 1 && FIELD_GET(CTRL_MODE, ctrl) == 3 &&
	      FIELD_GET(CTRL_DIV, ctrl) == 48, "registers",
	      "CTRL=0x%08x: EN=%lu MODE=%lu DIV=%lu (48 MHz / 1 MHz)", ctrl,
	      FIELD_GET(CTRL_EN, ctrl), FIELD_GET(CTRL_MODE, ctrl), FIELD_GET(CTRL_DIV, ctrl));

	/* unbind -> remove() + devm releases; then bind again -> probe() */
	device_release_driver(dev);
	check(seen.removes == 1 && !dev->driver && !strcmp(seen.release_order, "CBA"),
	      "unbind", "remove() ran, devm released in order %s (reverse of A,B,C)",
	      seen.release_order);
	seen.priv = NULL;
	ret = device_attach(dev);
	check(ret == 1 && seen.probes == 2 && seen.priv, "rebind",
	      "device_attach() = %d, probe count %d", ret, seen.probes);
}

static int __init platdev_init(void)
{
	int err;

	/* driver first: then device registration probes synchronously */
	err = platform_driver_register(&demo_driver);
	if (err)
		return err;
	err = demo_board_register();
	if (err) {
		platform_driver_unregister(&demo_driver);
		return err;
	}

	self_test();
	if (nr_fail)
		pr_err("done: %d check(s) FAILED\n", nr_fail);
	else
		pr_info("done: all checks passed; device stays bound until rmmod\n");
	/* Return 0 even on FAIL so the module can be inspected and rmmod'ed. */
	return 0;
}

static void __exit platdev_exit(void)
{
	demo_board_unregister();		/* device gone -> remove() */
	platform_driver_unregister(&demo_driver);
	pr_info("unloaded (probes %d, removes %d)\n", seen.probes, seen.removes);
}

module_init(platdev_init);
module_exit(platdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Self-test: platform device+driver, software-node properties, resources, devm, sysfs");
