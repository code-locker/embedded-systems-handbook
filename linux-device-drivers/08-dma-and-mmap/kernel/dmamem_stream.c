// SPDX-License-Identifier: GPL-2.0
/*
 * dmamem_stream.c — streaming DMA mappings and their OWNERSHIP rules,
 *                   scatter-gather mapping and dma_pool, as a load-time test
 *
 * Concept : A coherent buffer (dmamem_demo.c) is shared by CPU and device
 *           all the time. A STREAMING mapping instead lends an ordinary
 *           kernel buffer (kmalloc, a page, an skb, a pinned user page) to
 *           the device for one transfer, and ownership moves:
 *
 *     CPU owns     dma_map_*()    DEVICE owns     dma_unmap_*()  CPU owns
 *     -----------------+-----------------------------+-----------------
 *     CPU may touch    | CPU must NOT touch          | CPU may touch
 *                      |  dma_sync_*_for_cpu()    --> CPU owns (for a while)
 *                      |  dma_sync_*_for_device() <-- device owns again
 *
 *           On a non-coherent ARM device the map/sync calls are where the
 *           cache maintenance happens (clean before the device reads,
 *           invalidate before the CPU reads). Touching the buffer while
 *           the device owns it is the #1 DMA bug: it works on x86 and on
 *           coherent devices, and corrupts data on the others.
 *
 *           Tests: 1. map TO_DEVICE + FROM_DEVICE and (if a DMA_MEMCPY
 *           channel exists, e.g. K3 UDMA on the board) let the engine copy
 *           src -> dst, else only exercise the API; 2. BIDIRECTIONAL
 *           buffer with sync_for_cpu / sync_for_device round trip;
 *           3. dma_map_sg of scattered pages; 4. dma_pool of small aligned
 *           blocks. PASS/FAIL to dmesg; init returns 0.
 * Build   : make drivers   (or: make -C linux-device-drivers/08-dma-and-mmap/kernel)
 * Run     : sudo insmod dmamem_stream.ko [len=4096] ; dmesg ; sudo rmmod dmamem_stream
 * Read    : ../README.md  (section 3.4 "Streaming mappings and ownership")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/dmapool.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#define SG_PAGES 4
#define POOL_BLOCKS 16
#define POOL_SIZE 64	/* e.g. hardware descriptors */

static unsigned int len = 4096;
module_param(len, uint, 0444);
MODULE_PARM_DESC(len, "bytes per streaming buffer, 64..1048576");

static int failures;

static __printf(2, 3) void report(bool ok, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (ok) {
		pr_info("PASS %pV\n", &vaf);
	} else {
		failures++;
		pr_err("FAIL %pV\n", &vaf);
	}
	va_end(args);
}

static void dma_done(void *param)
{
	complete(param);
}

/* Copy src->dst with the engine; both already mapped for chan's device. */
static int engine_copy(struct dma_chan *chan, dma_addr_t dst, dma_addr_t src,
		       size_t n)
{
	struct dma_async_tx_descriptor *tx;
	DECLARE_COMPLETION_ONSTACK(done);
	dma_cookie_t cookie;

	tx = dmaengine_prep_dma_memcpy(chan, dst, src, n, DMA_PREP_INTERRUPT);
	if (!tx)
		return -EIO;
	tx->callback = dma_done;
	tx->callback_param = &done;
	cookie = dmaengine_submit(tx);
	if (dma_submit_error(cookie))
		return -EIO;
	dma_async_issue_pending(chan);
	if (!wait_for_completion_timeout(&done, msecs_to_jiffies(1000))) {
		dmaengine_terminate_sync(chan);
		return -ETIMEDOUT;
	}
	return 0;
}

/* ---- test 1: TO_DEVICE / FROM_DEVICE single mappings --------------------- */
static void test_single(struct device *dev, struct dma_chan *chan)
{
	struct device *mdev = chan ? dmaengine_get_dma_device(chan) : dev;
	dma_addr_t src_dma, dst_dma;
	u8 *src, *dst;
	unsigned int i;
	int ret = 0;

	src = kmalloc(len, GFP_KERNEL);		/* kmalloc: DMA-safe, aligned */
	dst = kzalloc(len, GFP_KERNEL);
	if (!src || !dst) {
		report(false, "single: out of memory");
		goto out;
	}
	for (i = 0; i < len; i++)
		src[i] = i ^ 0x5a;	/* CPU writes BEFORE map: allowed */

	/* map = "device, it's yours". TO_DEVICE: caches are CLEANED. */
	src_dma = dma_map_single(mdev, src, len, DMA_TO_DEVICE);
	if (dma_mapping_error(mdev, src_dma)) {	/* ALWAYS check */
		report(false, "single: map src");
		goto out;
	}
	/* FROM_DEVICE: lines are invalidated so no dirty line is written
	 * back over what the device writes.
	 */
	dst_dma = dma_map_single(mdev, dst, len, DMA_FROM_DEVICE);
	if (dma_mapping_error(mdev, dst_dma)) {
		dma_unmap_single(mdev, src_dma, len, DMA_TO_DEVICE);
		report(false, "single: map dst");
		goto out;
	}
	/*
	 * dma_addr_t vs physical address: equal without IOMMU/offset/bounce.
	 * Different = SMMU translation, a bus offset, or a swiotlb bounce
	 * buffer (e.g. a 32-bit mask on a >4 GiB buffer, or swiotlb=force).
	 */
	pr_info("INFO single: src phys %pa -> dma %pad (%s)\n",
		&(phys_addr_t){ virt_to_phys(src) }, &src_dma,
		virt_to_phys(src) == src_dma ? "identity" : "translated/bounced");

	/* ------- device owns src and dst: the CPU must not touch them ------ */
	if (chan)
		ret = engine_copy(chan, dst_dma, src_dma, len);
	/* ------------------------------------------------------------------- */

	/* unmap = "CPU, it's yours again". FROM_DEVICE: invalidate again
	 * (speculative prefetches may have pulled stale lines meanwhile).
	 */
	dma_unmap_single(mdev, dst_dma, len, DMA_FROM_DEVICE);
	dma_unmap_single(mdev, src_dma, len, DMA_TO_DEVICE);

	if (!chan)
		report(true, "single: map/unmap TO/FROM_DEVICE (no DMA_MEMCPY channel: API only)");
	else
		report(!ret && !memcmp(src, dst, len),
		       "single: %s copied %u bytes src -> dst through streaming mappings (ret %d)",
		       dma_chan_name(chan), len, ret);
out:
	kfree(dst);
	kfree(src);
}

/* ---- test 2: BIDIRECTIONAL + sync round trip ----------------------------- */
static void test_sync(struct device *dev)
{
	dma_addr_t h;
	u8 *buf = kmalloc(len, GFP_KERNEL);
	bool ok;

	if (!buf) {
		report(false, "sync: out of memory");
		return;
	}
	memset(buf, 0x11, len);
	h = dma_map_single(dev, buf, len, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, h)) {
		report(false, "sync: map");
		kfree(buf);
		return;
	}
	/* ...device would work on it here... then the CPU wants a look: */
	dma_sync_single_for_cpu(dev, h, len, DMA_BIDIRECTIONAL);
	ok = buf[0] == 0x11 && buf[len - 1] == 0x11;	/* CPU owns: OK */
	buf[0] = 0x22;					/* CPU edits a header */
	dma_sync_single_for_device(dev, h, len, DMA_BIDIRECTIONAL);
	/* ...device owns again... */
	dma_unmap_single(dev, h, len, DMA_BIDIRECTIONAL);
	ok = ok && buf[0] == 0x22 && buf[1] == 0x11;
	report(ok, "sync: map, sync_for_cpu, CPU edit, sync_for_device, unmap: data intact");
	kfree(buf);
}

/* ---- test 3: scatter-gather ---------------------------------------------- */
static void test_sg(struct device *dev)
{
	struct scatterlist sgl[SG_PAGES], *sg;
	struct page *pg[SG_PAGES] = { };
	int i, nents;
	bool ok = true;

	sg_init_table(sgl, SG_PAGES);
	for (i = 0; i < SG_PAGES; i++) {
		pg[i] = alloc_page(GFP_KERNEL);
		if (!pg[i]) {
			ok = false;
			goto out;
		}
		sg_set_page(&sgl[i], pg[i], PAGE_SIZE, 0);
	}
	/*
	 * Returns the number of DMA segments, which can be FEWER than the
	 * CPU entries: an IOMMU can map scattered pages to one contiguous
	 * bus range. Walk the result with the returned count, unmap with
	 * the ORIGINAL count. 0 = failure.
	 */
	nents = dma_map_sg(dev, sgl, SG_PAGES, DMA_TO_DEVICE);
	if (!nents) {
		ok = false;
		goto out;
	}
	for_each_sg(sgl, sg, nents, i)
		pr_info("INFO sg: segment %d: dma %pad len %u\n", i,
			&sg_dma_address(sg), sg_dma_len(sg));
	dma_unmap_sg(dev, sgl, SG_PAGES, DMA_TO_DEVICE);	/* original count */
out:
	for (i = 0; i < SG_PAGES; i++)
		if (pg[i])
			__free_page(pg[i]);
	report(ok, "sg: %d pages mapped as %d DMA segment(s)", SG_PAGES, ok ? nents : 0);
}

/* ---- test 4: dma_pool ---------------------------------------------------- */
static void test_pool(struct device *dev)
{
	struct dma_pool *pool;
	void *v[POOL_BLOCKS] = { };
	dma_addr_t h[POOL_BLOCKS];
	bool ok = true;
	int i;

	/*
	 * Small coherent blocks (descriptors) carved out of coherent pages:
	 * dma_alloc_coherent() works in pages, a pool in bytes.
	 */
	pool = dma_pool_create("dmamem_stream", dev, POOL_SIZE, POOL_SIZE, 0);
	if (!pool) {
		report(false, "pool: create");
		return;
	}
	for (i = 0; i < POOL_BLOCKS; i++) {
		v[i] = dma_pool_zalloc(pool, GFP_KERNEL, &h[i]);
		if (!v[i] || (h[i] & (POOL_SIZE - 1)))
			ok = false;
	}
	for (i = 0; i < POOL_BLOCKS; i++)
		if (v[i])
			dma_pool_free(pool, v[i], h[i]);
	dma_pool_destroy(pool);
	report(ok, "pool: %d blocks of %d bytes, every dma address %d-byte aligned",
	       POOL_BLOCKS, POOL_SIZE, POOL_SIZE);
}

/* ---- device + init ------------------------------------------------------- */

static int dmamem_stream_run(void)
{
	struct platform_device_info info = {
		.name = "dmamem-stream",
		.id = PLATFORM_DEVID_NONE,
		.dma_mask = DMA_BIT_MASK(32),
	};
	struct platform_device *pdev;
	struct dma_chan *chan;
	dma_cap_mask_t mask;
	int ret;

	/* A struct device to map for; no driver needed for the DMA API. */
	pdev = platform_device_register_full(&info);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto out;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	chan = dma_request_chan_by_mask(&mask);
	if (IS_ERR(chan)) {
		pr_info("INFO no DMA_MEMCPY channel (%ld): device transfer skipped\n",
			PTR_ERR(chan));
		chan = NULL;
	}

	test_single(&pdev->dev, chan);
	if (chan)
		dma_release_channel(chan);
	test_sync(&pdev->dev);
	test_sg(&pdev->dev);
	test_pool(&pdev->dev);
out:
	platform_device_unregister(pdev);
	return ret;
}

static int __init dmamem_stream_init(void)
{
	int ret;

	if (len < 64 || len > 1048576) {
		pr_err("len=%u out of range 64..1048576\n", len);
		return -EINVAL;
	}
	ret = dmamem_stream_run();
	if (ret)
		pr_err("setup failed: %d\n", ret);
	else if (failures)
		pr_err("done: %d check(s) FAILED\n", failures);
	else
		pr_info("done: all checks passed\n");
	return 0;	/* stay loaded so it can be rmmod-ed like the others */
}
module_init(dmamem_stream_init);

static void __exit dmamem_stream_exit(void)
{
}
module_exit(dmamem_stream_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("embedded-systems-handbook");
MODULE_DESCRIPTION("Lesson 08: streaming DMA mappings, ownership/sync rules, sg, dma_pool");
