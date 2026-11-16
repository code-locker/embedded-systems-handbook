// SPDX-License-Identifier: GPL-2.0
/*
 * kmb_multi_stats.c — second source file of kmb_multi.ko: integer statistics
 *
 * Concept : No floating point in the kernel. The FPU/NEON registers belong
 *           to the user task that was interrupted and are not saved on
 *           kernel entry, so kernel code keeps to integers (fixed point,
 *           "value x 100") unless it brackets FP code with
 *           kernel_neon_begin()/kernel_fpu_begin(). Also, 64-bit division
 *           with '/' fails to link on 32-bit ARM ("__aeabi_uldivmod
 *           undefined"): portable kernel code uses div_u64() & friends.
 *           This file is built with -DDEBUG (CFLAGS_kmb_multi_stats.o in
 *           the Makefile), so its pr_debug() prints without dynamic debug.
 * Build   : make drivers   (kmb_multi-y := kmb_multi_main.o kmb_multi_stats.o)
 * Read    : ../README.md  (section 3.7 "Kbuild")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/math64.h>
#include <linux/printk.h>
#include <linux/string.h>

#include "kmb_multi_stats.h"

void kmb_stats_init(struct kmb_stats *s, u32 bucket_width)
{
	memset(s, 0, sizeof(*s));
	s->min = U32_MAX;
	s->bucket_width = bucket_width ? bucket_width : 1;	/* never divide by 0 */
	/* Visible without dyndbg because this file is compiled with -DDEBUG. */
	pr_debug("stats: %d buckets of width %u\n", KMB_HIST_BUCKETS,
		 s->bucket_width);
}

/* O(1) per sample. */
void kmb_stats_add(struct kmb_stats *s, u32 v)
{
	u32 b = v / s->bucket_width;	/* 32-bit division: fine everywhere */

	if (b >= KMB_HIST_BUCKETS)
		b = KMB_HIST_BUCKETS - 1;
	s->hist[b]++;
	s->n++;
	s->sum += v;
	if (v < s->min)
		s->min = v;
	if (v > s->max)
		s->max = v;
}

/* Mean in fixed point: 50.5 is returned as 5050. */
u32 kmb_stats_mean_x100(const struct kmb_stats *s)
{
	if (!s->n)
		return 0;
	/* u64 / u32: div_u64() instead of '/' (see header comment). */
	return (u32)div_u64(s->sum * 100, s->n);
}
