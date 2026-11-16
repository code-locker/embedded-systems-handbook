// SPDX-License-Identifier: GPL-2.0
/*
 * kmb_consumer.c — a module that calls functions exported by kmb_lib.ko
 *
 * Concept : kmb_consumer.ko contains UNDEFINED references to
 *           kmb_lib_fletcher16 etc. (see `nm kmb_consumer.ko | grep ' U '`).
 *           At insmod time the loader resolves them against the exported
 *           symbols of the running kernel + loaded modules. If kmb_lib is
 *           not loaded the load fails:
 *             insmod: ERROR: could not insert module kmb_consumer.ko: Unknown symbol in module
 *             dmesg: kmb_consumer: Unknown symbol kmb_lib_fletcher16 (err -2)
 *           `modprobe kmb_consumer` (after installing + depmod) loads
 *           kmb_lib first because depmod found the dependency.
 * Build   : make drivers
 * Run     : sudo insmod kmb_lib.ko && sudo insmod kmb_consumer.ko text="hi"
 *           cat /sys/module/kmb_lib/holders/   -> kmb_consumer
 *           sudo rmmod kmb_consumer && sudo rmmod kmb_lib
 * Read    : ../README.md  (section 3.6 "Exporting symbols between modules")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/string.h>

#include "kmb_lib.h"

static char *text = "BeagleBone AI-64";
module_param(text, charp, 0444);
MODULE_PARM_DESC(text, "string to checksum with kmb_lib");

static int __init kmb_consumer_init(void)
{
	unsigned long before, after;
	u16 sum;
	int failed = 0;

	if (!text) {
		pr_err("text must be set\n");
		return -EINVAL;
	}

	/* Calls into ANOTHER module: an ordinary C call once linked. */
	before = kmb_lib_calls();
	sum = kmb_lib_fletcher16("abcde", 5);
	pr_info("INFO using %s; fletcher16(\"%s\") = 0x%04x\n",
		kmb_lib_version(), text,
		kmb_lib_fletcher16(text, strlen(text)));
	after = kmb_lib_calls();

	if (sum == 0xC8F0) {
		pr_info("PASS call      kmb_lib_fletcher16(\"abcde\") = 0x%04x (known answer)\n",
			sum);
	} else {
		pr_err("FAIL call      got 0x%04x, want 0xc8f0\n", sum);
		failed++;
	}

	/*
	 * >= rather than ==: another module may call kmb_lib concurrently.
	 * The counter lives in kmb_lib's memory; we only reach it through its
	 * exported function — exactly like a shared library's API.
	 */
	if (after >= before + 2) {
		pr_info("PASS state     kmb_lib call counter %lu -> %lu (state lives in kmb_lib)\n",
			before, after);
	} else {
		pr_err("FAIL state     counter %lu -> %lu\n", before, after);
		failed++;
	}

	if (failed)
		pr_err("done: %d check(s) FAILED\n", failed);
	else
		pr_info("done: all checks passed; kmb_lib now cannot be removed before us\n");
	return 0;
}

static void __exit kmb_consumer_exit(void)
{
	pr_info("unloaded; kmb_lib may be removed now\n");
}

module_init(kmb_consumer_init);
module_exit(kmb_consumer_exit);

/* kmb_lib_version() lives in namespace KMB (KMB_NS: see kmb_lib.h). */
MODULE_IMPORT_NS(KMB_NS);

MODULE_LICENSE("GPL");		/* required: the exports are _GPL */
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Module basics: consumer of kmb_lib's exported symbols");
MODULE_VERSION("1.0");
