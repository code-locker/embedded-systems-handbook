// SPDX-License-Identifier: GPL-2.0
/*
 * dmamem_fault.c — /dev/dmamem_fault: mmap of NON-contiguous kernel pages,
 *                  page by page, with a .fault handler or vm_insert_page()
 *
 * Concept : remap_pfn_range() maps one physically CONTIGUOUS range in one go.
 *           Many driver buffers are not contiguous (vmalloc, alloc_page per
 *           page, a scatter list of video frames). For those you map page by
 *           page:
 *             mode=0  lazy: .mmap only installs vm_ops; the first touch of
 *                     each page faults, .fault hands the struct page to the
 *                     MM (vmf->page) — pages never touched cost nothing
 *             mode=1  eager: .mmap calls vm_insert_page() for every page
 *           This is the host-safe variant of lesson 08: no DMA at all, just
 *           ordinary cached RAM (contrast with dmamem_demo.c, which maps a
 *           coherent DMA buffer with dma_mmap_coherent()). read()/write()
 *           access the same pages, so user space can check that both views
 *           agree.
 * Build   : make drivers   (or: make -C linux-device-drivers/08-dma-and-mmap/kernel)
 * Run     : sudo insmod dmamem_fault.ko [pages=16] [mode=0]
 *           sudo ./build/linux-device-drivers/08-dma-and-mmap/user/dmamem_test
 *           sudo rmmod dmamem_fault      (prints the number of faults served)
 * Read    : ../README.md  (section 3.8 "mmap in drivers")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

static unsigned int pages = 16;
module_param(pages, uint, 0444);
MODULE_PARM_DESC(pages, "buffer size in pages, 1..1024");

static unsigned int mode;
module_param(mode, uint, 0444);
MODULE_PARM_DESC(mode, "0 = map lazily from .fault, 1 = vm_insert_page() all pages in .mmap");

/*
 *  pagev[0] -> page @ pfn 0x8a3f1         user VMA (virtually contiguous)
 *  pagev[1] -> page @ pfn 0x91002         [ p0 | p1 | p2 | ... ]
 *  pagev[2] -> page @ pfn 0x8a3f7           each PTE points to whatever
 *  ...           (scattered)                 physical page pagev[i] is
 */
static struct page **pagev;
static atomic_t faults = ATOMIC_INIT(0);

static vm_fault_t dmamem_fault(struct vm_fault *vmf)
{
	/* vmf->pgoff: page index inside the FILE (vma->vm_pgoff included) */
	if (vmf->pgoff >= pages)
		return VM_FAULT_SIGBUS;

	/*
	 * Give the MM a reference; it drops it when the PTE goes away
	 * (munmap/exit), so the page outlives a racing module free.
	 */
	get_page(pagev[vmf->pgoff]);
	vmf->page = pagev[vmf->pgoff];
	atomic_inc(&faults);
	return 0;
}

static const struct vm_operations_struct dmamem_vm_ops = {
	.fault = dmamem_fault,
};

static int dmamem_fault_mmap(struct file *f, struct vm_area_struct *vma)
{
	unsigned long npages = vma_pages(vma);
	unsigned long i;
	int ret;

	if (vma->vm_pgoff >= pages || npages > pages - vma->vm_pgoff)
		return -EINVAL;
	/* MAP_PRIVATE would copy-on-write our pages away from the driver */
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	/*
	 * DONTEXPAND: mremap() may not grow it past our buffer.
	 * DONTDUMP:   keep driver memory out of core dumps.
	 * (Since 6.3 vm_flags is read-only: use vm_flags_set().)
	 */
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

	if (mode == 0) {
		vma->vm_ops = &dmamem_vm_ops;	/* pages arrive on first touch */
		return 0;
	}
	for (i = 0; i < npages; i++) {
		/* takes its own page reference; needs order-0, non-slab pages */
		ret = vm_insert_page(vma, vma->vm_start + i * PAGE_SIZE,
				     pagev[vma->vm_pgoff + i]);
		if (ret)
			return ret;	/* the VMA is torn down by the caller */
	}
	return 0;
}

/* read()/write(): the same pages through copy_to_user/copy_from_user. */
static ssize_t dmamem_fault_rw(char __user *ubuf, size_t count, loff_t *ppos,
			       bool write)
{
	size_t size = (size_t)pages * PAGE_SIZE, done = 0;
	loff_t pos = *ppos;

	if (pos < 0)
		return -EINVAL;
	if (pos >= size)
		return 0;
	count = min_t(size_t, count, size - pos);

	while (done < count) {
		size_t off = offset_in_page(pos);
		size_t n = min_t(size_t, count - done, PAGE_SIZE - off);
		/* page_address(): lowmem pages always have a kernel mapping
		 * on 64-bit; on 32-bit with HIGHMEM you would kmap_local_page().
		 */
		void *kaddr = page_address(pagev[pos >> PAGE_SHIFT]) + off;
		unsigned long left;

		left = write ? copy_from_user(kaddr, ubuf + done, n) :
			       copy_to_user(ubuf + done, kaddr, n);
		if (left) {
			done += n - left;
			break;
		}
		done += n;
		pos += n;
	}
	*ppos += done;
	return done ? done : -EFAULT;
}

static ssize_t dmamem_fault_read(struct file *f, char __user *buf, size_t count,
				 loff_t *ppos)
{
	return dmamem_fault_rw(buf, count, ppos, false);
}

static ssize_t dmamem_fault_write(struct file *f, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	return dmamem_fault_rw((char __user *)buf, count, ppos, true);
}

static const struct file_operations dmamem_fault_fops = {
	.owner = THIS_MODULE,
	.mmap = dmamem_fault_mmap,
	.read = dmamem_fault_read,
	.write = dmamem_fault_write,
	.llseek = default_llseek,
};

static struct miscdevice dmamem_fault_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "dmamem_fault",
	.fops = &dmamem_fault_fops,
	.mode = 0600,
};

static void free_pagev(unsigned int n)
{
	while (n--)
		__free_page(pagev[n]);	/* drops OUR reference only */
	kfree(pagev);
}

static int __init dmamem_fault_init(void)
{
	unsigned int i;
	int ret;

	if (!pages || pages > 1024 || mode > 1) {
		pr_err("pages=%u (1..1024) mode=%u (0..1) out of range\n", pages, mode);
		return -EINVAL;
	}
	pagev = kcalloc(pages, sizeof(*pagev), GFP_KERNEL);
	if (!pagev)
		return -ENOMEM;
	for (i = 0; i < pages; i++) {
		/* zeroed: never hand old kernel data to user space */
		pagev[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!pagev[i]) {
			free_pagev(i);
			return -ENOMEM;
		}
	}
	ret = misc_register(&dmamem_fault_misc);
	if (ret) {
		free_pagev(pages);
		return ret;
	}
	/* page frame numbers: pfn << PAGE_SHIFT = physical address */
	pr_info("/dev/dmamem_fault: %u separate pages, mode %u (%s); first two at pfn 0x%lx, 0x%lx\n",
		pages, mode, mode ? "vm_insert_page in mmap" : ".fault on first touch",
		page_to_pfn(pagev[0]), page_to_pfn(pagev[pages > 1 ? 1 : 0]));
	return 0;
}
module_init(dmamem_fault_init);

static void __exit dmamem_fault_exit(void)
{
	misc_deregister(&dmamem_fault_misc);
	pr_info("%d page faults served\n", atomic_read(&faults));
	free_pagev(pages);
}
module_exit(dmamem_fault_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("embedded-systems-handbook");
MODULE_DESCRIPTION("Lesson 08: mmap of scattered pages via .fault or vm_insert_page");
