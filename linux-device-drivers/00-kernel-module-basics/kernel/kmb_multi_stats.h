/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kmb_multi_stats.h — interface between the two files of kmb_multi.ko
 *
 * Concept : kmb_multi_main.c and kmb_multi_stats.c are compiled separately
 *           and linked into ONE module (kmb_multi-y := ... in the Makefile).
 *           Non-static functions declared here are visible across the two
 *           files but NOT to other modules: only EXPORT_SYMBOL does that.
 * Read    : ../README.md  (section 3.7 "Kbuild")
 */
#ifndef KMB_MULTI_STATS_H
#define KMB_MULTI_STATS_H

#include <linux/types.h>

/* Set for this whole directory by `ccflags-y` in the Makefile. */
#ifndef KMB_HIST_BUCKETS
#error "KMB_HIST_BUCKETS must come from ccflags-y in kernel/Makefile"
#endif

struct kmb_stats {
	u32 n;
	u32 min;
	u32 max;
	u64 sum;			/* u64: n * max must not overflow */
	u32 bucket_width;
	u32 hist[KMB_HIST_BUCKETS];	/* last bucket also takes overflows */
};

void kmb_stats_init(struct kmb_stats *s, u32 bucket_width);
void kmb_stats_add(struct kmb_stats *s, u32 v);
u32 kmb_stats_mean_x100(const struct kmb_stats *s);

#endif /* KMB_MULTI_STATS_H */
