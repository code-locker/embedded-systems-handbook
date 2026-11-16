// SPDX-License-Identifier: GPL-2.0
/*
 * dmamem_demo.c — /dev/dmamem: a coherent DMA buffer that user space can
 *                 mmap(), and an ioctl that copies inside it with a DMA engine
 *
 * Concept : The full path a camera/video/SPI driver uses for its buffers:
 *             1. a struct device with a DMA mask (dma_set_mask_and_coherent)
 *             2. dma_alloc_coherent(): CPU address + dma_addr_t for the device
 *             3. .mmap = dma_mmap_coherent(): the SAME memory in user space,
 *                with the SAME cache attributes the kernel mapping has
 *             4. dmaengine "memcpy" (DMA_MEMCPY capability): prep, submit,
 *                issue_pending, completion callback
 *           On the BeagleBone AI-64 the copy runs on a K3 UDMA channel; on a
 *           PC or VM without a memcpy-capable DMA controller it falls back to
 *           a CPU memcpy() and says so — the ioctl reports which one ran.
 *
 *           There is no real hardware here, so the module registers its own
 *           platform_device (with a 32-bit DMA mask) and a platform_driver
 *           for it — the device model part is lesson 06. If a DMA channel is
 *           found, the buffer is allocated for the CHANNEL's device, because
 *           that is the device that will master the bus.
 * Build   : make drivers   (or: make -C linux-device-drivers/08-dma-and-mmap/kernel)
 * Run     : sudo insmod dmamem_demo.ko [buf_kb=64] [use_dma=1]
 *           sudo ./build/linux-device-drivers/08-dma-and-mmap/user/dmamem_test
 *           sudo rmmod dmamem_demo
 * Read    : ../README.md  (sections 3.3 "Coherent buffers", 3.6 "dmaengine",
 *           3.8 "mmap in drivers")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/version.h>

/* ---- user-space ABI (copied verbatim in user/dmamem_test.c) -------------- */
struct dmamem_info {
	__u64 size;		/* buffer size in bytes (mmap at most this) */
	__u32 dmaengine;	/* 1 = a DMA_MEMCPY channel is in use */
	__u32 reserved;
	char chan_name[32];	/* e.g. "dma0chan3", or "none" */
};

struct dmamem_copy {
	__u64 src_off;		/* in:  byte offsets inside the buffer */
	__u64 dst_off;
	__u64 len;
	__u32 used_dmaengine;	/* out: 1 = DMA engine, 0 = CPU memcpy */
	__u32 reserved;
	__u64 nsec;		/* out: time for the copy incl. waiting */
};

#define DMAMEM_IOC_MAGIC 'D'
#define DMAMEM_IOC_INFO	_IOR(DMAMEM_IOC_MAGIC, 1, struct dmamem_info)
#define DMAMEM_IOC_COPY	_IOWR(DMAMEM_IOC_MAGIC, 2, struct dmamem_copy)
/* -------------------------------------------------------------------------- */

#define DRV_NAME "dmamem-demo"
#define DMA_TIMEOUT_MS 1000

static unsigned int buf_kb = 64;
module_param(buf_kb, uint, 0444);
MODULE_PARM_DESC(buf_kb, "buffer size in KiB, 4..4096 (rounded up to whole pages)");

static bool use_dma = true;
module_param(use_dma, bool, 0444);
MODULE_PARM_DESC(use_dma, "try to get a DMA_MEMCPY channel (0 = always CPU memcpy)");

struct dmamem {
	struct device *dev;		/* our platform device */
	struct device *dma_dev;		/* device the buffer is mapped for */
	struct dma_chan *chan;		/* NULL = CPU fallback */
	void *cpu;			/* kernel virtual address */
	dma_addr_t dma;			/* address as seen by dma_dev */
	size_t size;
	struct mutex lock;		/* one copy at a time */
	struct miscdevice misc;
};

/*
 *   kernel:   d->cpu  ─────┐
 *   user:     mmap() ──────┼──>  the same physical pages (one allocation)
 *   device:   d->dma ──────┘     dma_addr_t: may differ from the physical
 *                                address (IOMMU/SMMU, bus offsets)
 */

/* ---- the copy: dmaengine or CPU ----------------------------------------- */

static void dmamem_dma_done(void *param)
{
	complete(param);	/* runs in the DMA driver's tasklet/irq thread */
}

static int dmamem_copy_dma(struct dmamem *d, size_t dst, size_t src, size_t len)
{
	struct dma_async_tx_descriptor *tx;
	DECLARE_COMPLETION_ONSTACK(done);
	enum dma_status status;
	dma_cookie_t cookie;

	/* 1. Prepare: the driver builds hardware descriptors (UDMA: a TR). */
	tx = dmaengine_prep_dma_memcpy(d->chan, d->dma + dst, d->dma + src, len,
				       DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!tx)
		return -EIO;
	tx->callback = dmamem_dma_done;
	tx->callback_param = &done;

	/* 2. Submit: queued on the channel, NOT started; gives a cookie. */
	cookie = dmaengine_submit(tx);
	if (dma_submit_error(cookie))
		return -EIO;

	/* 3. Issue: now the hardware starts on everything queued. */
	dma_async_issue_pending(d->chan);

	if (!wait_for_completion_timeout(&done, msecs_to_jiffies(DMA_TIMEOUT_MS))) {
		/* Stop the channel and wait for its callbacks before 'done'
		 * (on our stack) goes out of scope.
		 */
		dmaengine_terminate_sync(d->chan);
		return -ETIMEDOUT;
	}
	status = dmaengine_tx_status(d->chan, cookie, NULL);
	return status == DMA_COMPLETE ? 0 : -EIO;
}

static int dmamem_copy(struct dmamem *d, struct dmamem_copy *c)
{
	ktime_t t0 = ktime_get();
	int ret = -ENODEV;

	c->used_dmaengine = 0;
	if (d->chan && is_dma_copy_aligned(d->chan->device, c->src_off,
					   c->dst_off, c->len)) {
		ret = dmamem_copy_dma(d, c->dst_off, c->src_off, c->len);
		if (!ret)
			c->used_dmaengine = 1;
		else
			dev_warn(d->dev, "DMA copy failed (%d), using CPU memcpy\n", ret);
	}
	if (ret) {
		dev_info_ratelimited(d->dev, "CPU memcpy fallback for %llu bytes (%s)\n",
				     c->len, d->chan ? "DMA failed or unaligned" :
						       "no DMA_MEMCPY channel");
		/*
		 * Coherent memory: no cache maintenance needed. On a
		 * non-coherent arm64 device this mapping is uncached
		 * (Normal-NC), so the CPU copy is slow — one more reason the
		 * engine exists.
		 */
		memcpy(d->cpu + c->dst_off, d->cpu + c->src_off, c->len);
	}
	c->nsec = ktime_to_ns(ktime_sub(ktime_get(), t0));
	return 0;
}

/* ---- file operations ---------------------------------------------------- */

static struct dmamem *file_dmamem(struct file *f)
{
	/* misc core stores the miscdevice in private_data at open() */
	return container_of(f->private_data, struct dmamem, misc);
}

static int dmamem_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct dmamem *d = file_dmamem(f);

	/* a private (copy-on-write) mapping of device memory makes no sense */
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	/*
	 * dma_mmap_coherent() checks pgoff+length against the size, picks
	 * the right page protection (cached for coherent devices,
	 * pgprot_dmacoherent = uncached otherwise) and remaps the pages.
	 * Never remap_pfn_range(virt_to_phys(cpu)) a coherent buffer
	 * yourself: with an IOMMU or remapped allocations that address is
	 * wrong, and the cache attributes may mismatch the kernel's alias.
	 */
	return dma_mmap_coherent(d->dma_dev, vma, d->cpu, d->dma, d->size);
}

static long dmamem_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct dmamem *d = file_dmamem(f);
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case DMAMEM_IOC_INFO: {
		struct dmamem_info info = { };	/* zeroed: no kernel stack leak */

		info.size = d->size;
		info.dmaengine = !!d->chan;
		strscpy(info.chan_name, d->chan ? dma_chan_name(d->chan) : "none",
			sizeof(info.chan_name));
		return copy_to_user(uarg, &info, sizeof(info)) ? -EFAULT : 0;
	}
	case DMAMEM_IOC_COPY: {
		struct dmamem_copy c;
		int ret;

		if (copy_from_user(&c, uarg, sizeof(c)))
			return -EFAULT;
		/* Validate everything that came from user space, overflow-safe. */
		if (!c.len || c.len > d->size ||
		    c.src_off > d->size - c.len || c.dst_off > d->size - c.len)
			return -EINVAL;
		/* memcpy semantics: ranges must not overlap */
		if (c.src_off < c.dst_off + c.len && c.dst_off < c.src_off + c.len)
			return -EINVAL;

		mutex_lock(&d->lock);
		ret = dmamem_copy(d, &c);
		mutex_unlock(&d->lock);
		if (ret)
			return ret;
		return copy_to_user(uarg, &c, sizeof(c)) ? -EFAULT : 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations dmamem_fops = {
	.owner = THIS_MODULE,	/* module pinned while a file (or mmap) is open */
	.mmap = dmamem_mmap,
	.unlocked_ioctl = dmamem_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

/* ---- self-test at probe -------------------------------------------------- */

static void dmamem_selftest(struct dmamem *d)
{
	struct dmamem_copy c = { .src_off = 0, .dst_off = d->size / 2,
				 .len = d->size / 2 };
	u8 *p = d->cpu;
	size_t i;

	for (i = 0; i < c.len; i++)
		p[i] = (u8)(i * 7 + 1);
	memset(p + c.dst_off, 0, c.len);

	mutex_lock(&d->lock);
	dmamem_copy(d, &c);
	mutex_unlock(&d->lock);

	if (memcmp(p, p + c.dst_off, c.len))
		dev_err(d->dev, "FAIL selftest: copy of %llu bytes differs\n", c.len);
	else
		dev_info(d->dev, "PASS selftest: %llu bytes copied by %s in %llu ns\n",
			 c.len, c.used_dmaengine ? "DMA engine" : "CPU memcpy", c.nsec);
	memset(p, 0, d->size);
}

/* ---- probe / remove ------------------------------------------------------ */

static int dmamem_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dmamem *d;
	dma_cap_mask_t mask;
	int ret;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->dev = dev;
	d->size = PAGE_ALIGN((size_t)buf_kb * 1024);
	mutex_init(&d->lock);

	/*
	 * "This device can address 32 bits of bus address space." The DMA
	 * layer then allocates/bounces so every dma_addr_t fits. A real
	 * driver states what its hardware can do (e.g. 48 for many K3 IPs).
	 */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "no usable 32-bit DMA\n");

	if (use_dma) {
		dma_cap_zero(mask);
		dma_cap_set(DMA_MEMCPY, mask);
		/* any channel with the capability; no DT "dmas" needed */
		d->chan = dma_request_chan_by_mask(&mask);
		if (IS_ERR(d->chan)) {
			dev_info(dev, "no DMA_MEMCPY channel (%ld): CPU memcpy fallback\n",
				 PTR_ERR(d->chan));
			d->chan = NULL;
		}
	}
	/* Map for whoever masters the bus: the DMA controller if we have one. */
	d->dma_dev = d->chan ? dmaengine_get_dma_device(d->chan) : dev;

	d->cpu = dma_alloc_coherent(d->dma_dev, d->size, &d->dma, GFP_KERNEL);
	if (!d->cpu) {
		ret = -ENOMEM;
		goto err_chan;
	}

	d->misc.minor = MISC_DYNAMIC_MINOR;
	d->misc.name = "dmamem";		/* /dev/dmamem */
	d->misc.fops = &dmamem_fops;
	d->misc.parent = dev;
	d->misc.mode = 0600;
	platform_set_drvdata(pdev, d);

	dev_info(dev, "%zu KiB coherent buffer, dma %pad, mapped for %s, copy via %s\n",
		 d->size / 1024, &d->dma, dev_name(d->dma_dev),
		 d->chan ? dma_chan_name(d->chan) : "CPU memcpy");
	dmamem_selftest(d);

	/* Register LAST: from here on user space can call into us. */
	ret = misc_register(&d->misc);
	if (ret)
		goto err_free;
	return 0;

err_free:
	dma_free_coherent(d->dma_dev, d->size, d->cpu, d->dma);
err_chan:
	if (d->chan)
		dma_release_channel(d->chan);
	return ret;
}

static void dmamem_remove(struct platform_device *pdev)
{
	struct dmamem *d = platform_get_drvdata(pdev);

	/* Reverse order of probe. No file can be open here: fops.owner holds
	 * the module and .suppress_bind_attrs forbids a sysfs unbind.
	 */
	misc_deregister(&d->misc);
	dma_free_coherent(d->dma_dev, d->size, d->cpu, d->dma);
	if (d->chan)
		dma_release_channel(d->chan);
}

static struct platform_driver dmamem_driver = {
	.probe = dmamem_probe,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	.remove = dmamem_remove,	/* void-returning .remove since 6.11 */
#else
	.remove_new = dmamem_remove,
#endif
	.driver = {
		.name = DRV_NAME,
		/* unbind while user space has the buffer mmap()ed = use after free */
		.suppress_bind_attrs = true,
	},
};

static struct platform_device *dmamem_pdev;

static int __init dmamem_init(void)
{
	struct platform_device_info info = {
		.name = DRV_NAME,
		.id = PLATFORM_DEVID_NONE,
		.dma_mask = DMA_BIT_MASK(32),	/* sets dev.dma_mask + coherent mask */
	};
	int ret;

	if (buf_kb < 4 || buf_kb > 4096) {
		pr_err("buf_kb=%u out of range 4..4096\n", buf_kb);
		return -EINVAL;
	}
	ret = platform_driver_register(&dmamem_driver);
	if (ret)
		return ret;
	dmamem_pdev = platform_device_register_full(&info);
	if (IS_ERR(dmamem_pdev)) {
		platform_driver_unregister(&dmamem_driver);
		return PTR_ERR(dmamem_pdev);
	}
	return 0;
}
module_init(dmamem_init);

static void __exit dmamem_exit(void)
{
	platform_device_unregister(dmamem_pdev);	/* -> dmamem_remove() */
	platform_driver_unregister(&dmamem_driver);
}
module_exit(dmamem_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("embedded-systems-handbook");
MODULE_DESCRIPTION("Lesson 08: coherent DMA buffer, dma_mmap_coherent, dmaengine memcpy with CPU fallback");
