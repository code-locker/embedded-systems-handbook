// SPDX-License-Identifier: GPL-2.0
/*
 * dmamem_bug.c — INTENTIONALLY BROKEN: streaming-DMA mistakes, and which of
 *                them CONFIG_DMA_API_DEBUG can and cannot see
 *
 * Concept : bug=1  CPU writes a TO_DEVICE buffer AFTER dma_map_single() and
 *                  before the device reads it, without
 *                  dma_sync_single_for_device(). On x86 (coherent) it
 *                  "works". On a non-coherent ARM device the new bytes may
 *                  still sit in the CPU cache, and with a bounce buffer
 *                  (boot with swiotlb=force, or a 32-bit mask over a >4 GiB
 *                  buffer) the device ALWAYS sees the old bytes, because the
 *                  copy into the bounce buffer happened at map time. If a
 *                  DMA_MEMCPY channel exists the module lets the engine copy
 *                  the buffer and reports what the device saw.
 *                  DMA-API debug does NOT catch this: it tracks API calls,
 *                  not CPU loads/stores. Only review, swiotlb=force testing
 *                  and real non-coherent hardware find it.
 *           bug=2  no dma_mapping_error() check after mapping ->
 *                  "DMA-API: ... device driver failed to check map error"
 *           bug=3  unmap with a different direction than the map ->
 *                  "DMA-API: ... device driver frees DMA memory with
 *                   different direction"
 *           bug=4  unmap with a different size ->
 *                  "DMA-API: ... device driver frees DMA memory with
 *                   different size"
 *           bugs 2-4 need a kernel with CONFIG_DMA_API_DEBUG=y (off on the
 *           board's and Ubuntu's kernels); see /sys/kernel/debug/dma-api/.
 * Build   : make -C linux-device-drivers/08-dma-and-mmap/kernel BUGS=1
 *           (NOT built by default, and not by `make drivers`)
 * Run     : ONLY in a VM / on the board: sudo insmod dmamem_bug.ko bug=1 ;
 *           dmesg ; sudo rmmod dmamem_bug
 * Read    : ../README.md  (section 7 "Common mistakes and pitfalls")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define BUG_LEN 4096

static int bug = 1;
module_param(bug, int, 0444);
MODULE_PARM_DESC(bug, "1 = CPU write after map w/o sync, 2 = no mapping_error check, 3 = wrong unmap direction, 4 = wrong unmap size");

static void done_cb(void *p)
{
	complete(p);
}

static void bug_write_after_map(struct device *dev)
{
	struct device *mdev = dev;
	struct dma_async_tx_descriptor *tx;
	DECLARE_COMPLETION_ONSTACK(done);
	dma_addr_t s, d;
	struct dma_chan *chan;
	dma_cap_mask_t mask;
	u8 *src, *dst;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	chan = dma_request_chan_by_mask(&mask);
	if (IS_ERR(chan))
		chan = NULL;
	else
		mdev = dmaengine_get_dma_device(chan);

	src = kmalloc(BUG_LEN, GFP_KERNEL);
	dst = kzalloc(BUG_LEN, GFP_KERNEL);
	if (!src || !dst)
		goto out;
	memset(src, 0xaa, BUG_LEN);			/* "old" data */

	s = dma_map_single(mdev, src, BUG_LEN, DMA_TO_DEVICE);
	d = dma_map_single(mdev, dst, BUG_LEN, DMA_FROM_DEVICE);
	if (dma_mapping_error(mdev, s) || dma_mapping_error(mdev, d))
		goto out;		/* (leaks a mapping: fine for a bug demo) */

	memset(src, 0x55, BUG_LEN);	/* BUG: device owns src now, no sync */
	/* FIX: dma_sync_single_for_device(mdev, s, BUG_LEN, DMA_TO_DEVICE); */

	if (chan) {
		tx = dmaengine_prep_dma_memcpy(chan, d, s, BUG_LEN, DMA_PREP_INTERRUPT);
		if (tx) {
			tx->callback = done_cb;
			tx->callback_param = &done;
			dmaengine_submit(tx);
			dma_async_issue_pending(chan);
			if (!wait_for_completion_timeout(&done, HZ))
				dmaengine_terminate_sync(chan);
		}
	}
	dma_unmap_single(mdev, d, BUG_LEN, DMA_FROM_DEVICE);
	dma_unmap_single(mdev, s, BUG_LEN, DMA_TO_DEVICE);

	if (!chan)
		pr_info("bug 1: no DMA_MEMCPY channel, nothing observed the stale data — the bug is still there\n");
	else
		pr_info("bug 1: device copied 0x%02x (0x55 = new data, 0xaa = STALE data reached the device)\n",
			dst[0]);
out:
	kfree(dst);
	kfree(src);
	if (chan)
		dma_release_channel(chan);
}

static void bug_api_misuse(struct device *dev, int which)
{
	u8 *buf = kmalloc(BUG_LEN, GFP_KERNEL);
	dma_addr_t h;

	if (!buf)
		return;
	h = dma_map_single(dev, buf, BUG_LEN, DMA_TO_DEVICE);
	if (which != 2 && dma_mapping_error(dev, h))
		goto out;
	/* bug 2: h used without dma_mapping_error() */

	switch (which) {
	case 3:
		dma_unmap_single(dev, h, BUG_LEN, DMA_FROM_DEVICE);	/* BUG */
		break;
	case 4:
		dma_unmap_single(dev, h, BUG_LEN / 2, DMA_TO_DEVICE);	/* BUG */
		break;
	default:
		dma_unmap_single(dev, h, BUG_LEN, DMA_TO_DEVICE);
	}
	pr_info("bug %d done: with CONFIG_DMA_API_DEBUG=y a \"DMA-API:\" warning is above\n",
		which);
out:
	kfree(buf);
}

static int __init dmamem_bug_init(void)
{
	struct platform_device_info info = {
		.name = "dmamem-bug",
		.id = PLATFORM_DEVID_NONE,
		.dma_mask = DMA_BIT_MASK(32),
	};
	struct platform_device *pdev;

	if (bug < 1 || bug > 4) {
		pr_err("bug=%d: use 1..4\n", bug);
		return -EINVAL;
	}
	pdev = platform_device_register_full(&info);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	if (bug == 1)
		bug_write_after_map(&pdev->dev);
	else
		bug_api_misuse(&pdev->dev, bug);
	platform_device_unregister(pdev);
	return 0;
}
module_init(dmamem_bug_init);

static void __exit dmamem_bug_exit(void)
{
}
module_exit(dmamem_bug_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("embedded-systems-handbook");
MODULE_DESCRIPTION("Lesson 08: intentionally broken streaming DMA usage");
