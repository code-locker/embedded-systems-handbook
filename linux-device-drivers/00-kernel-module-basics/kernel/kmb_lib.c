// SPDX-License-Identifier: GPL-2.0
/*
 * kmb_lib.c — a "library" module: exports functions for other modules
 *
 * Concept : EXPORT_SYMBOL_GPL(f) puts f in the kernel's symbol table so a
 *           module loaded LATER can call it. Loading a user of f creates a
 *           dependency: kmb_lib cannot be removed while kmb_consumer is
 *           loaded ("Module kmb_lib is in use by: kmb_consumer"), and
 *           depmod records it in modules.dep so `modprobe kmb_consumer`
 *           loads kmb_lib first. One symbol is exported into a namespace
 *           ("KMB"), which importers must declare with MODULE_IMPORT_NS.
 * Build   : make drivers   (both modules are built in the same M= dir, so
 *           modpost sees kmb_lib's exports when it checks kmb_consumer)
 * Run     : sudo insmod kmb_lib.ko && sudo insmod kmb_consumer.ko
 *           grep kmb_lib /proc/kallsyms ; lsmod | grep kmb_
 *           sudo rmmod kmb_consumer kmb_lib     (order matters)
 * Read    : ../README.md  (section 3.6 "Exporting symbols between modules")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/export.h>
#include <linux/module.h>

#include "kmb_lib.h"

/*
 * Module-private state. `static` keeps the NAME private to this file;
 * only exported symbols are visible to other modules. Every module shares
 * one kernel address space, so a unique prefix (kmb_) on every global still
 * matters: it is what you see in /proc/kallsyms, oopses and ftrace.
 */
static atomic_long_t kmb_calls = ATOMIC_LONG_INIT(0);

/*
 * Fletcher-16: two running sums mod 255. O(n) time, O(1) space.
 * Callable from any context (no sleeping, no locks): document that in a
 * real exported API, callers cannot see your implementation.
 *
 * EXPORT_SYMBOL_GPL (right after the function, kernel style): only modules
 * with a GPL-compatible MODULE_LICENSE may link to it. Plain EXPORT_SYMBOL
 * would allow any license. New kernel APIs are almost always _GPL.
 */
u16 kmb_lib_fletcher16(const void *buf, size_t len)
{
	const u8 *p = buf;
	u32 s1 = 0, s2 = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		s1 = (s1 + p[i]) % 255;
		s2 = (s2 + s1) % 255;
	}
	atomic_long_inc(&kmb_calls);	/* concurrent callers on 2 CPUs: atomic */
	return (u16)(s2 << 8 | s1);
}
EXPORT_SYMBOL_GPL(kmb_lib_fletcher16);

unsigned long kmb_lib_calls(void)
{
	return atomic_long_read(&kmb_calls);
}
EXPORT_SYMBOL_GPL(kmb_lib_calls);

/*
 * Symbol namespaces: an importer must say MODULE_IMPORT_NS(KMB_NS) or
 * modpost fails the build ("uses symbol ... from namespace KMB, but does
 * not import it"). KMB_NS is defined in kmb_lib.h (string vs token, 6.13).
 */
const char *kmb_lib_version(void)
{
	return "kmb_lib 1.0";
}
EXPORT_SYMBOL_NS_GPL(kmb_lib_version, KMB_NS);

static int __init kmb_lib_init(void)
{
	/* Known answers (Fletcher-16 test vectors). */
	static const struct {
		const char *in;
		u16 sum;
	} vec[] = {
		{ "abcde", 0xC8F0 }, { "abcdef", 0x2057 }, { "abcdefgh", 0x0627 },
	};
	int i, bad = 0;

	for (i = 0; i < ARRAY_SIZE(vec); i++) {
		u16 got = kmb_lib_fletcher16(vec[i].in, strlen(vec[i].in));

		if (got != vec[i].sum) {
			pr_err("FAIL fletcher16(\"%s\") = 0x%04x, want 0x%04x\n",
			       vec[i].in, got, vec[i].sum);
			bad++;
		}
	}
	if (!bad)
		pr_info("PASS selftest  %zu Fletcher-16 vectors; exports ready for other modules\n",
			ARRAY_SIZE(vec));
	return 0;
}

static void __exit kmb_lib_exit(void)
{
	/*
	 * The module core only lets us get here when no module that uses
	 * our symbols is loaded (its refcount on us is 0).
	 */
	pr_info("unloaded after %lu checksum calls\n", kmb_lib_calls());
}

module_init(kmb_lib_init);
module_exit(kmb_lib_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Module basics: a library module that exports GPL symbols");
MODULE_VERSION("1.0");
