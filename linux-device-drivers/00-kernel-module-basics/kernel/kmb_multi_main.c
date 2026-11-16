// SPDX-License-Identifier: GPL-2.0
/*
 * kmb_multi_main.c — first source file of kmb_multi.ko (two .c -> one .ko)
 *
 * Concept : In the Makefile
 *               obj-m       += kmb_multi.o
 *               kmb_multi-y := kmb_multi_main.o kmb_multi_stats.o
 *           tells Kbuild: there is no kmb_multi.c; link these two objects
 *           into kmb_multi.ko. The module name must differ from every
 *           source file name. module_init/exit and the MODULE_* tags may
 *           appear in only one of the files.
 * Build   : make drivers
 * Run     : sudo insmod kmb_multi.ko samples=5000 ; sudo rmmod kmb_multi
 * Read    : ../README.md  (section 3.7 "Kbuild")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>

#include "kmb_multi_stats.h"

static unsigned int samples = 1000;
module_param(samples, uint, 0444);
MODULE_PARM_DESC(samples, "pseudo-random samples for the demo run (1..100000)");

/*
 * Tiny deterministic LCG (Numerical Recipes constants): the same sequence on
 * every run, so the demo is reproducible. Not get_random_u32(): a self-test
 * wants repeatable input.
 */
static u32 kmb_lcg(u32 *state)
{
	*state = *state * 1664525u + 1013904223u;
	return *state >> 16;
}

static int __init kmb_multi_init(void)
{
	/* ~56 bytes: small enough for the stack (keep locals far below 1 KiB). */
	struct kmb_stats st;
	u32 seed = 1, i, mean;
	int b, failed = 0;
	bool hist_ok = true;

	if (samples < 1 || samples > 100000) {
		pr_err("samples=%u out of range 1..100000\n", samples);
		return -EINVAL;
	}

	pr_info("INFO one module from two objects: kmb_multi_main.o + kmb_multi_stats.o, %d buckets\n",
		KMB_HIST_BUCKETS);

	/* Test 1: known answer, 0..(8*10-1) with width 10 -> 10 per bucket. */
	kmb_stats_init(&st, 10);
	for (i = 0; i < KMB_HIST_BUCKETS * 10; i++)
		kmb_stats_add(&st, i);
	for (b = 0; b < KMB_HIST_BUCKETS; b++)
		hist_ok &= st.hist[b] == 10;
	mean = kmb_stats_mean_x100(&st);
	if (hist_ok && st.min == 0 && st.max == KMB_HIST_BUCKETS * 10 - 1 &&
	    mean == (KMB_HIST_BUCKETS * 10 - 1) * 50) {
		pr_info("PASS stats     n=%u min=%u max=%u mean=%u.%02u, every bucket = 10\n",
			st.n, st.min, st.max, mean / 100, mean % 100);
	} else {
		pr_err("FAIL stats     n=%u min=%u max=%u mean_x100=%u\n",
		       st.n, st.min, st.max, mean);
		failed++;
	}

	/* Test 2: an out-of-range sample lands in the last (overflow) bucket. */
	kmb_stats_add(&st, 100000);
	if (st.hist[KMB_HIST_BUCKETS - 1] == 11) {
		pr_info("PASS overflow  value 100000 counted in the last bucket\n");
	} else {
		pr_err("FAIL overflow  last bucket = %u\n", st.hist[KMB_HIST_BUCKETS - 1]);
		failed++;
	}

	/* Demo: pseudo-random values 0..99, integer-only statistics. */
	kmb_stats_init(&st, 100 / KMB_HIST_BUCKETS + 1);
	for (i = 0; i < samples; i++)
		kmb_stats_add(&st, kmb_lcg(&seed) % 100);
	mean = kmb_stats_mean_x100(&st);
	pr_info("INFO demo      %u samples: min=%u max=%u mean=%u.%02u (no floating point)\n",
		st.n, st.min, st.max, mean / 100, mean % 100);

	if (failed)
		pr_err("done: %d check(s) FAILED\n", failed);
	else
		pr_info("done: all checks passed\n");
	return 0;
}

static void __exit kmb_multi_exit(void)
{
	pr_info("unloaded\n");
}

module_init(kmb_multi_init);
module_exit(kmb_multi_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Module basics: one module built from two source files");
MODULE_VERSION("1.0");
