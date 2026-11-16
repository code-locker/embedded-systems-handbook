# 08 — DMA and mmap

> A camera on the BeagleBone AI-64 produces 1920×1080×2 bytes 30 times a
> second, and the CPU should not copy any of it. This lesson is how
> drivers let hardware move data on its own (**DMA**) and how they hand
> the same memory to user space without copying (**mmap**): bus addresses
> and `dma_addr_t`, the SMMU, cache coherency on ARM, the DMA mask,
> coherent vs streaming mappings and their ownership rules, `dma_pool`,
> CMA, the dmaengine API (K3 UDMA on the TDA4VM), DMA-BUF, and the
> `.mmap` file operation in its three forms. Three self-testing modules
> and a user program put it together: a coherent buffer mmap()ed by user
> space and copied by the DMA engine on the board, or by `memcpy()` on a
> PC.

---

## 1. Why this exists

Copying data with the CPU costs CPU time and memory bandwidth, and
evicts useful data from the caches. For a 1080p30 YUV stream that is
~125 MB/s just to move frames, before anybody looks at them. Every
serious peripheral therefore has a **DMA** (direct memory access)
capability: it reads and writes RAM itself and interrupts when done.
On the TDA4VM: the CSI-2 camera receivers, the video codecs, the display,
Ethernet, McSPI, MMC and the generic **UDMA** engine all master the bus.

DMA brings three problems the CPU never had:

1. **Addresses**: the device does not see the CPU's virtual addresses,
   and not always the physical ones (IOMMU/SMMU, bus offsets, 32-bit
   limits).
2. **Caches**: the CPU writes into its cache, the device reads RAM. On a
   **non-coherent** system it reads stale data unless somebody cleans the
   cache first, and the other way round for device writes.
3. **Ownership**: while the device works on a buffer, the CPU must not
   touch it, and vice versa.

The kernel's **DMA API** (`<linux/dma-mapping.h>`) solves all three in
one portable interface; the **dmaengine** API drives generic DMA
controllers; **mmap** removes the last copy, the one between kernel and
user space. The user-space side of zero-copy (`mmap`, `splice`,
`io_uring`, DMA-BUF fds) is in
[linux-internals/16-zero-copy-and-async-io](../../linux-internals/16-zero-copy-and-async-io/)
and, for video, [gstreamer/13-performance-zero-copy](../../gstreamer/13-performance-zero-copy/).

---

## 2. Mental model

**A buffer is a shared workbench, and DMA is a second worker who cannot
see your notes.** Coherent memory is a bench under a glass roof: both
of you see everything at once, but working there is slower (uncached on
non-coherent systems). Streaming memory is your own fast desk that you
**lend** to the other worker: before lending you tidy it (cache clean),
while it is lent you keep your hands off, and when it comes back you
throw away your memory of what was on it (cache invalidate).

```
  CPU (A72)            L1/L2 caches            interconnect            RAM
  virtual addr  -MMU-> physical addr  ------------------------------>  [ buffer ]
                                                                          ^
  device (UDMA, CSI)   dma_addr_t  --(SMMU/IOMMU if used)-> physical -----'
                       (a "bus address", what the DMA API gives you)

  non-coherent device: does NOT snoop the CPU caches
      CPU wrote X -> X sits dirty in cache -> device reads RAM: OLD value
      device wrote Y -> CPU cache still holds old line -> CPU reads: OLD value
  coherent device (dma-coherent in DT): hardware snoops caches, problem gone
```

The sentence to remember: **give the device a `dma_addr_t`, never a
pointer; and between `map` and `unmap`, the buffer belongs to the device.**

---

## 3. How it works

### 3.1 Addresses: virtual, physical, bus (`dma_addr_t`), IOMMU/SMMU

| Address | Who uses it | Type | Get it with |
|---|---|---|---|
| kernel virtual | CPU code in the kernel | `void *` | `kmalloc`, `dma_alloc_coherent` return value |
| user virtual | a process | `void *` in user space | `mmap()` |
| physical | the CPU's MMU output | `phys_addr_t` | `virt_to_phys()` (linear map only), `page_to_phys()` |
| bus / DMA | the device | `dma_addr_t` | **only** from the DMA API: `dma_map_*`, `dma_alloc_coherent` |

The DMA address can differ from the physical one because of an **IOMMU**
(called **SMMU** on ARM): an MMU in front of a device, with its own page
tables. It lets a device with 32-bit addressing reach memory above 4 GiB,
makes a scattered buffer look contiguous to the device, and stops a
broken or malicious device from writing anywhere (isolation, also for
VFIO and VMs). The TDA4VM has an **SMMUv3** (`smmu0: iommu@36600000` in
`k3-j721e-main.dtsi`, `CONFIG_ARM_SMMU_V3=y` in the board kernel), but in
the board's device tree **no master has an `iommus` property**, so DMA
addresses are physical addresses there (`dmamem_stream` prints both so
you can check). Other reasons for a difference: a fixed bus offset
(`dma-ranges` in DT) and **swiotlb bounce buffers** (the DMA API copies
the data to a low buffer the device can reach; `swiotlb=force` on the
kernel command line forces it for every mapping, which is a great way to
find ownership bugs).

### 3.2 Cache coherency on ARM, and the DMA mask

x86 PCs are cache-coherent for DMA. On ARM SoCs each master either
snoops the CPU caches (coherent) or not. The DT says which: the property
`dma-coherent` on the device or a parent bus. On the TDA4VM the NAVSS
bus (`main_navss: bus@30000000`) carries `dma-coherent`, so the UDMA
engines in it are coherent; many other masters are not. The DMA API reads
the property (`dev_is_dma_coherent()`) and does maintenance only when
needed:

```
 streaming map, non-coherent device, arm64 (arch_sync_dma_for_device/cpu):

 dma_map_single(.., DMA_TO_DEVICE)      CLEAN   dirty lines -> RAM (dc cvac)
                                                device reads RAM: sees CPU data
 dma_map_single(.., DMA_FROM_DEVICE)    INVALIDATE  (dc ivac / civac)
                                                so no dirty line is evicted
                                                ON TOP of what the device writes
 ... device writes RAM ...
 dma_unmap_single(.., DMA_FROM_DEVICE)  INVALIDATE again: lines the CPU
                                                SPECULATIVELY prefetched during
                                                the transfer are stale
 CPU reads: misses the cache, gets the device's data
```

Coherent allocations on a non-coherent device are mapped **uncached**
(Normal non-cacheable on arm64) in the kernel and, through
`dma_mmap_coherent`, in user space, so there is nothing to maintain, at
the price of slow CPU access.

**DMA mask**: how many address bits the device can drive.

```c
ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));   /* or 48, 64 */
if (ret) /* this platform cannot satisfy it: fail or try a smaller mask */
```

The DMA layer then makes sure every `dma_addr_t` it returns fits (by
allocating low memory, using the IOMMU, or bouncing). Set it in probe,
before any mapping. DT-created platform devices get a default 32-bit mask;
`dmamem_demo` creates its own platform device with
`platform_device_info.dma_mask = DMA_BIT_MASK(32)` and calls
`dma_set_mask_and_coherent` like a real driver. (`dma_coerce_mask_and_coherent`
exists for devices whose `dma_mask` pointer was never set up; avoid it in
new code.)

### 3.3 Coherent buffers  (`kernel/dmamem_demo.c`)

```c
void *cpu = dma_alloc_coherent(dev, size, &dma_handle, GFP_KERNEL);
/* CPU uses cpu, the device uses dma_handle, at any time, no syncs */
dma_free_coherent(dev, size, cpu, dma_handle);
/* or dmam_alloc_coherent(): freed automatically on unbind */
```

Use it for long-lived memory both sides touch all the time: descriptor
rings, mailboxes, small control blocks, and buffers that are
mmap()ed to user space. Allocations are page-granular and come from CMA
or the page allocator; the CPU mapping may be uncached (non-coherent
devices) or a vmalloc-style remap.

**Small blocks: `dma_pool`** (`kernel/dmamem_stream.c` test 4): hardware
descriptors of 32–64 bytes with alignment rules would waste a page each.

```c
pool = dma_pool_create("desc", dev, 64 /*size*/, 64 /*align*/, 0 /*boundary*/);
d = dma_pool_zalloc(pool, GFP_KERNEL, &d_dma);
dma_pool_free(pool, d, d_dma);
dma_pool_destroy(pool);
```

**CMA, the contiguous memory allocator.** A device without an IOMMU
(the AI-64's camera and display path, as configured) needs a physically
contiguous buffer: 4 MiB for one 1080p YUV422 frame, times the number of
buffers in the pipeline. After hours of uptime the page allocator cannot
find that much contiguous memory. CMA reserves a region at boot that the
page allocator may lend out for **movable** pages; when a driver asks, the
pages are migrated away and the range is handed over. The board kernel
has `CONFIG_CMA=y` with a 144 MiB default (`CONFIG_CMA_SIZE_MBYTES`);
change it with `cma=256M` on the kernel command line, or with a
`shared-dma-pool` + `reusable` reserved-memory node in DT for one device.
`dma_alloc_coherent` uses CMA automatically for large allocations; see
`grep -i cma /proc/meminfo`.

### 3.4 Streaming mappings and ownership  (`kernel/dmamem_stream.c`)

For buffers that already exist (a kmalloc'd packet, a page-cache page, an
skb, pinned user pages) and are used for one transfer:

```c
dma_addr_t h = dma_map_single(dev, buf, len, DMA_TO_DEVICE);
if (dma_mapping_error(dev, h))     /* ALWAYS: mapping can fail (IOMMU, swiotlb full) */
	return -ENOMEM;
/* ---- device owns buf: start the transfer, wait for the IRQ ---- */
dma_unmap_single(dev, h, len, DMA_TO_DEVICE);   /* same dev, h, len, direction */
/* ---- CPU owns buf again ---- */
```

| Direction | Device... | Map does (non-coherent) | Unmap does |
|---|---|---|---|
| `DMA_TO_DEVICE` | reads the buffer | clean | nothing |
| `DMA_FROM_DEVICE` | writes the buffer | invalidate | invalidate |
| `DMA_BIDIRECTIONAL` | both | clean + invalidate | invalidate |

**Ownership rules** (the #1 source of DMA bugs, because breaking them
works on x86 and on coherent devices, and corrupts data on the others):

```
   CPU owns        map          DEVICE owns              unmap        CPU owns
 ------------------>|<-------------------------------------->|------------------>
   CPU may R/W      |  CPU must not read or write             |  CPU may R/W
                    |   sync_for_cpu()  --> CPU owns for a while
                    |   sync_for_device() <-- device owns again
```

To look at a mapped buffer without unmapping (e.g. a receive ring that
stays mapped forever), use the sync calls:

```c
dma_sync_single_for_cpu(dev, h, len, DMA_FROM_DEVICE);    /* CPU may read */
process(buf);
dma_sync_single_for_device(dev, h, len, DMA_FROM_DEVICE); /* give it back */
```

**Scatter-gather**: `dma_map_sg(dev, sgl, nents, dir)` maps a list of
pages and returns the number of **DMA segments**, possibly fewer than
`nents` (an IOMMU merges them). Program the device with the returned
count via `for_each_sg(sgl, sg, count, i) { sg_dma_address(sg); sg_dma_len(sg); }`,
but unmap with the **original** `nents`. `dma_map_sgtable()` /
`dma_unmap_sgtable()` with a `struct sg_table` avoid that confusion.
Also `dma_map_page()` for one page.

**Not DMA-able**: stack memory (`CONFIG_VMAP_STACK` puts it in vmalloc
space), `vmalloc` memory (not physically contiguous; map its pages one by
one), module `.data`/`.bss`, anything sharing a cache line with
unrelated data (on non-coherent systems an invalidate destroys the
neighbour; `kmalloc` aligns to `ARCH_DMA_MINALIGN` for that reason).

**Debugging**: `CONFIG_DMA_API_DEBUG=y` (off in Ubuntu's and the board's
kernels) checks API usage: unmap with the wrong size or direction, missing
`dma_mapping_error()` checks, double unmap, freeing a mapped buffer,
mappings leaked at driver unbind. Reports start with `DMA-API:`;
`/sys/kernel/debug/dma-api/` has counters and a per-driver filter. It
**cannot** see a CPU store into a buffer the device owns; for that, test
with `swiotlb=force` (the device always sees the bounce copy made at map
time) and on real non-coherent hardware. `kernel/dmamem_bug.c` shows
both kinds.

### 3.5 Which allocation for which job

| Need | Use |
|---|---|
| long-lived, both sides touch it, maybe mmap to user | `dma_alloc_coherent` / `dmam_alloc_coherent` |
| many small DMA blocks (descriptors) | `dma_pool` |
| existing kernel buffer, one transfer | `dma_map_single` / `dma_map_page` |
| scattered pages (user pages, page cache) | `dma_map_sg` / `dma_map_sgtable` |
| big contiguous frames, no IOMMU | `dma_alloc_coherent` (CMA behind it) or a dma-heap from user space |
| user buffer straight to a device | `pin_user_pages()` + `dma_map_sgtable()` |
| share one buffer between drivers/processes | DMA-BUF (3.7) |

### 3.6 The dmaengine API  (`kernel/dmamem_demo.c`, `dmamem_stream.c`)

A **DMA controller** (provider) offers channels; peripheral drivers
(consumers) request one and queue descriptors on it. On the TDA4VM the
provider is the K3 **UDMA** (`k3-udma` driver, `ti,j721e-navss-main-udmap`
and `-mcu-udmap`; newer K3 SoCs have BCDMA and PKTDMA with the same
driver), configured through TI-SCI resource ranges.

```
 consumer driver                              provider (k3-udma)
 chan = dma_request_chan(dev, "rx")    -----> channel from DT "dmas"/"dma-names"
   or dma_request_chan_by_mask(&mask)  -----> any channel with the capability (DMA_MEMCPY)
 dmaengine_slave_config(chan, &cfg)           peripheral FIFO address, width, burst
 tx = dmaengine_prep_slave_sg(chan, sgl, n, DMA_DEV_TO_MEM, flags)
   or dmaengine_prep_dma_memcpy(chan, dst, src, len, flags)   (builds hw descriptors / TRs)
 tx->callback = done; tx->callback_param = ...
 cookie = dmaengine_submit(tx)                queued, not started; check dma_submit_error()
 dma_async_issue_pending(chan)        -----> hardware starts on everything queued
 ... completion IRQ -> provider tasklet/thread -> done(param) -> complete()
 dmaengine_tx_status(chan, cookie, &state)    DMA_COMPLETE / IN_PROGRESS / ERROR (+ residue)
 dmaengine_terminate_sync(chan)               abort, wait until callbacks finished
 dma_release_channel(chan)
```

The DMA addresses must be valid for the device that **masters** the
transfer: for a memcpy that is the DMA controller
(`dmaengine_get_dma_device(chan)`), for a slave transfer the controller
too (the peripheral only holds the FIFO). That is why `dmamem_demo`
allocates its buffer for the channel's device when it has one.
`is_dma_copy_aligned(chan->device, ...)` tells whether offsets/length meet
the engine's `copy_align`.

Slave DMA in a real driver, e.g. McSPI or the CSI-2 receiver: the DT node
has `dmas = <&main_udmap 0xc600>, ...; dma-names = "rx0", "tx0";`, the driver
calls `dma_request_chan(dev, "rx0")`, `dmaengine_slave_config()` with the
peripheral register address, then `prep_slave_sg` per buffer, usually
with a cyclic or queued descriptor per frame.

`dma_request_chan_by_mask()` returning `-EPROBE_DEFER` (-517) means no
dmaengine provider is registered at all (typical on a PC); `-ENODEV`
(-19) means providers exist but none offers the capability. Either way
`dmamem_demo` logs it and uses `memcpy()`.

Kernel test module for providers: `dmatest` (`CONFIG_DMATEST`, not enabled
in the board config): `modprobe dmatest channel=dma0chan0 iterations=100 run=1`.

### 3.7 DMA-BUF: sharing buffers between drivers and processes

One camera frame is written by the CSI-2 receiver, read by the ISP
(VPAC), the encoder and the display, and touched by GStreamer in user
space. DMA-BUF makes that one buffer instead of five copies:

```
 exporter (allocates; e.g. a V4L2 capture driver, a dma-heap, a GPU driver)
     dma_buf_export(&exp_info)  ->  struct dma_buf  ->  dma_buf_fd()  -> fd to user space
                                                                         | passed around
 importer (another driver; e.g. DRM display, the encoder)               v
     dma_buf_get(fd) -> dma_buf_attach(dmabuf, dev)
     dma_buf_map_attachment(att, dir) -> sg_table already mapped FOR THE IMPORTER'S device
     ... DMA ...
     dma_buf_unmap_attachment(); dma_buf_detach(); dma_buf_put()
 synchronisation: dma_fence objects in the buffer's dma_resv: "the encoder
     may start when the camera's fence signals"; user space sees them as
     sync_file fds / implicit sync. CPU access from user space:
     mmap(fd) + ioctl(DMA_BUF_IOCTL_SYNC, START/END) around it.
```

User space allocates DMA-BUFs from `/dev/dma_heap/system` or
`/dev/dma_heap/linux,cma` (contiguous). GStreamer's `dmabuf` memory,
`v4l2src io-mode=dmabuf` and `kmssink` use this; see
[gstreamer/13](../../gstreamer/13-performance-zero-copy/) and
[gstreamer/14](../../gstreamer/14-embedded-tda4vm/).

### 3.8 mmap in drivers  (`dmamem_demo.c`, `dmamem_fault.c`, `user/dmamem_test.c`)

`mmap(2)` on a device file calls the driver's `.mmap(file, vma)`. The
VMA (`struct vm_area_struct`) already describes the user range
(`vm_start`, `vm_end`, `vm_pgoff` = offset in pages); the driver fills in
the page table entries, now or on fault.

```
  user VA  [vm_start ............................ vm_end)
              |  PTEs installed by one of:
              |    remap_pfn_range()   whole PHYSICALLY CONTIGUOUS range, now
              |    vm_insert_page()    one struct page at a time, now
              |    .fault handler      one page when first touched (lazy)
              |    dma_mmap_coherent() a dma_alloc_coherent buffer, right attributes
              v
  physical pages
```

| Method | Memory | Notes |
|---|---|---|
| `remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)` | physically contiguous RAM or MMIO | sets `VM_IO | VM_PFNMAP`; no `struct page` refcounting; check `size` and `vm_pgoff` yourself |
| `vm_insert_page()` / `vm_insert_pages()` | individual order-0 kernel pages (not slab) | the page gets a reference; works for scattered buffers |
| `.fault` → `vmf->page = page` (+ `get_page`) or `vmf_insert_pfn()` | anything, lazily | cost only for pages touched; can also implement "device memory appears when ready" |
| `dma_mmap_coherent(dev, vma, cpu, dma, size)` | `dma_alloc_coherent` buffers | uses the same cacheability as the kernel mapping; handles IOMMU/remapped buffers; never hand-roll this with `virt_to_phys` |
| `vm_iomap_memory(vma, phys, len)` | MMIO | the safe wrapper around `remap_pfn_range` with bounds checks |

Page protection for device memory:
`vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot)` (registers:
strongly ordered, every access goes to the device; on arm64 Device-nGnRnE)
or `pgprot_writecombine()` (frame buffers: writes may be merged and
buffered, much faster for streaming writes; arm64 Normal-NC).

Flags and checks every `.mmap` needs: refuse what is too big
(`vma_pages(vma)` + `vm_pgoff` against your size), refuse `MAP_PRIVATE`
for device memory (`!(vma->vm_flags & VM_SHARED)`: a private mapping would
copy-on-write away from the device), set `VM_DONTEXPAND | VM_DONTDUMP`
with `vm_flags_set()` (since 6.3 `vm_flags` is read-only). Lifetime: a
mapping keeps the **file** open, and `fops.owner = THIS_MODULE` keeps the
module loaded, but a driver **unbind** can still free the buffer under a
live mapping. `dmamem_demo` sets `suppress_bind_attrs`; real drivers
refcount the buffer, or zap the mappings (`unmap_mapping_range`) before
freeing.

**Registers to user space.** For a quick prototype, **UIO**
(`uio_pdrv_genirq`, enabled as a module on the board) maps a device's
`reg` ranges through `/dev/uioN` and delivers its interrupt as a blocking
`read()`; real drivers then belong in the kernel. `/dev/mem` (map any
physical address) is the dangerous shortcut: it bypasses every driver, can
lock up the bus by touching a powered-down block (common on the TDA4VM,
where blocks are power-gated by TI-SCI), and `CONFIG_STRICT_DEVMEM`
restricts it to non-RAM anyway.

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| `dma_alloc_coherent` | µs to ms | page allocation, maybe CMA migration and a remap; do it at probe, not per frame |
| `dma_pool_alloc` | ~100 ns | free list in pre-allocated pages |
| `dma_map_single`, coherent device / x86 | ~100 ns | address computation only |
| `dma_map_single`, non-coherent, n bytes | O(n / 64) cache-line ops | clean/invalidate every line; 4 MiB = 65536 lines |
| `dma_map_single` with swiotlb bounce | O(n) memcpy | why bounce buffers kill performance |
| `dma_map_sg` with IOMMU | O(pages) PTE writes + IOTLB | plus possible merging |
| dmaengine memcpy setup | a few µs | descriptor build, doorbell, IRQ, callback |
| dmaengine memcpy, n bytes | n / (engine bandwidth) | GB/s; the CPU is free meanwhile |
| CPU `memcpy` from uncached (coherent, non-coherent dev) memory | ~10× slower than cached | every load goes to DRAM |
| `remap_pfn_range` / `vm_insert_page` mmap | O(pages) at mmap time | PTEs written up front |
| `.fault` mmap | one fault (~1 µs) per page touched | lazy; untouched pages free |
| `read()` on a driver buffer | syscall + O(n) copy | what mmap avoids |

---

## 5. C vs C++ — and kernel vs user space

The kernel side is C. The user-space view and its equivalents:

| Kernel (driver) | User space |
|---|---|
| `dma_alloc_coherent` | `/dev/dma_heap/linux,cma` allocation → DMA-BUF fd |
| `.mmap` file operation | `mmap(fd, MAP_SHARED)` on the device / DMA-BUF fd |
| `dma_sync_single_for_cpu/device` | `ioctl(DMA_BUF_IOCTL_SYNC, DMA_BUF_SYNC_START/END)` |
| dmaengine memcpy | no direct access; ask a driver (this lesson's ioctl), or a TI DMA utility library on the R5/C7x side |
| `pgprot_writecombine` | `O_SYNC` on `/dev/mem` gives uncached, nothing gives WC directly |
| UIO | `/dev/uioN` + `mmap` + blocking `read()` for IRQs |

C++ in user space: wrap the fd and the mapping in RAII types
(`unique_fd`, a `Mapping` class whose destructor calls `munmap`) and expose
the buffer as `std::span<std::byte>`; libcamera and GStreamer's C++
bindings do exactly that for DMA-BUFs.

---

## 6. Where it is used in the real world

- **V4L2 / videobuf2** (`videobuf2-dma-contig.c`, `-dma-sg.c`): every camera
  driver, including the TDA4VM CSI-2 receiver (`j721e-csi2rx`, which uses
  UDMA through dmaengine), allocates frames with these and exports them
  as DMA-BUFs; `mmap` of a V4L2 buffer is `dma_mmap_coherent` underneath.
- **DRM/KMS** (`drm_gem_dma_helper.c`): scan-out buffers for the display;
  PRIME is DMA-BUF.
- **Network drivers** (`am65-cpsw-nuss` on K3): RX/TX rings in coherent
  memory, packets as streaming mappings, `dma_sync_single_for_cpu` on RX,
  page_pool for recycling.
- **SPI/MMC controllers**: `spi-omap2-mcspi` switches to UDMA slave DMA
  for long transfers; `spi_map_buf()` does the streaming mapping for you.
- **remoteproc / rpmsg**: R5F/C7x cores share `shared-dma-pool` reserved
  memory (see `mcu_r5fss0_core0_dma_memory_region` in the board DTS).
- **dma-heaps** (`/dev/dma_heap/*`): allocator for user-space pipelines
  (GStreamer, Android).
- **UIO / VFIO**: user-space drivers (DPDK, prototypes); VFIO uses the
  IOMMU to make that safe.

---

## 7. Common mistakes and pitfalls

1. **CPU touches a buffer between map and unmap without a sync**
   (`dmamem_bug.c bug=1`). Invisible on x86 and coherent devices; stale
   data elsewhere. Test with `swiotlb=force`.
2. **No `dma_mapping_error()` check** (`bug=2`). The returned handle can be
   an error cookie; the device then writes to address 0 or a random place.
3. **Unmapping with a different size/direction/device than mapping**
   (`bug=3`, `bug=4`); `CONFIG_DMA_API_DEBUG` catches it.
4. **Giving the device a virtual address, or `virt_to_phys()`**: wrong as
   soon as an IOMMU, a bus offset or swiotlb is involved. Only the DMA API's
   `dma_addr_t`.
5. **DMA from the stack, vmalloc or module data**: not DMA-able.
6. **Mapping for the wrong device**: with dmaengine the controller does the
   DMA; map/allocate for `dmaengine_get_dma_device(chan)`.
7. **Forgetting the DMA mask** or assuming 64-bit: allocations above 4 GiB
   for a 32-bit device, silent corruption or bounce-buffer slowness.
8. **Walking `dma_map_sg()` results with the original `nents`**, or
   unmapping with the returned count.
9. **`remap_pfn_range(virt_to_phys(cpu))` on a coherent buffer**: wrong
   for remapped/IOMMU buffers, and maps cacheable what the kernel maps
   uncached (attribute mismatch = undefined behaviour on ARM). Use
   `dma_mmap_coherent`.
10. **No size/offset check in `.mmap`**: user space maps past your buffer
    into other kernel memory.
11. **Freeing the buffer while user space still has it mapped** (unbind,
    error paths): use-after-free through the page tables.
12. **`MAP_PRIVATE` mappings of device memory**: copy-on-write gives the
    process a private copy; the device and the process silently diverge.
13. **Big contiguous allocations at runtime without CMA**: work after boot,
    fail after a day. Allocate at probe; size CMA (`cma=`).
14. **Waiting for a DMA completion with a callback on the stack and no
    `dmaengine_terminate_sync()` on timeout**: the late callback writes
    into a dead stack frame.

---

## 8. Files in this lesson

| File | What it shows | Build / run |
|---|---|---|
| [kernel/Makefile](kernel/Makefile) | Kbuild makefile: `KDIR=`, `ARCH=`/`CROSS_COMPILE=`, `BUGS=1`, `W=1`, `clean`; platform `.remove` handled for 6.8 and ≥ 6.11 | `make drivers` |
| [kernel/dmamem_demo.c](kernel/dmamem_demo.c) | own platform device + driver, `dma_set_mask_and_coherent`, `dma_request_chan_by_mask(DMA_MEMCPY)`, `dma_alloc_coherent` for the right device, `dma_mmap_coherent`, ioctl → `dmaengine_prep_dma_memcpy`/submit/issue_pending/callback with timeout + `terminate_sync`, CPU fallback, self-test | `insmod dmamem_demo.ko [buf_kb=64] [use_dma=1]` |
| [kernel/dmamem_stream.c](kernel/dmamem_stream.c) | streaming `dma_map_single` TO/FROM_DEVICE (real UDMA copy on the board), `dma_mapping_error`, phys vs `dma_addr_t`, BIDIRECTIONAL + `sync_for_cpu/device`, `dma_map_sg` segments, `dma_pool` alignment | `insmod dmamem_stream.ko [len=4096]` |
| [kernel/dmamem_fault.c](kernel/dmamem_fault.c) | host-safe mmap of scattered pages: `.fault` (`mode=0`) or `vm_insert_page` (`mode=1`), `vm_flags_set`, `MAP_SHARED` check, `read()`/`write()` on the same pages | `insmod dmamem_fault.ko [pages=16] [mode=0]` |
| [kernel/dmamem_bug.c](kernel/dmamem_bug.c) | **intentionally broken** (only with `BUGS=1`): CPU write after map w/o sync; no `dma_mapping_error`; wrong unmap direction; wrong unmap size | `make -C …/kernel BUGS=1`, `insmod dmamem_bug.ko bug=1` |
| [user/dmamem_test.c](user/dmamem_test.c) | `DMAMEM_IOC_INFO`, refused `MAP_PRIVATE`/oversized maps, `mmap(MAP_SHARED)`, pattern → `DMAMEM_IOC_COPY` (EINVAL for overlap/out of range) → verify through the mapping, prints engine vs memcpy; checks `/dev/dmamem_fault` mmap vs `pread`/`pwrite` | `sudo ./dmamem_test` |

`dmamem_test` exits **0** on success, **1** if a check fails, **2** if
`/dev/dmamem` is missing (with a hint). `make test` does not run it.

**Build** (from the repo root):

```sh
make drivers                                                  # host: modules + user/ program
make -C linux-device-drivers/08-dma-and-mmap/kernel W=1       # extra warnings
make -C linux-device-drivers/08-dma-and-mmap/kernel BUGS=1    # + dmamem_bug.ko
# board (paths: ../README.md#building)
make -C linux-device-drivers/08-dma-and-mmap/kernel \
     KDIR=$BB/KERNEL ARCH=arm64 CROSS_COMPILE=$BB/dl/gcc-15.2.0-nolibc/aarch64-linux/bin/aarch64-linux-
aarch64-linux-gnu-gcc -O2 -static -o dmamem_test linux-device-drivers/08-dma-and-mmap/user/dmamem_test.c
make drivers-clean
```

All modules are **build-tested only**: on the host (Ubuntu 22.04, kernel
6.8, zero warnings with and without `W=1`, checkpatch-clean apart from the
file-name note and the `LINUX_VERSION_CODE` warning for the platform
`.remove` guard, which an out-of-tree module that supports 6.8 and 6.18
needs) and cross-built against the board's `6.18.x-arm64-k3` PREEMPT_RT
tree with its GCC 15. Loading them is **your** step, in a VM or on the
board ([../README.md](../README.md#safety)). The output below is what the
code prints; nobody has captured it from a real run yet, so expect
differences in numbers, addresses and channel names.

**VM / PC kernel** (no memcpy-capable DMA engine):

```sh
sudo insmod linux-device-drivers/08-dma-and-mmap/kernel/dmamem_demo.ko
#   dmamem-demo dmamem-demo: no DMA_MEMCPY channel (-517): CPU memcpy fallback
#   dmamem-demo dmamem-demo: 64 KiB coherent buffer, dma 0x0000000012340000, mapped for dmamem-demo, copy via CPU memcpy
#   dmamem-demo dmamem-demo: CPU memcpy fallback for 32768 bytes (no DMA_MEMCPY channel)
#   dmamem-demo dmamem-demo: PASS selftest: 32768 bytes copied by CPU memcpy in 9000 ns
sudo insmod linux-device-drivers/08-dma-and-mmap/kernel/dmamem_fault.ko
sudo ./build/linux-device-drivers/08-dma-and-mmap/user/dmamem_test
#   /dev/dmamem: 65536 bytes, DMA channel: none
#   PASS copy: 32768 bytes by CPU memcpy (no DMA_MEMCPY channel) in 12 us, verified through the mapping
#   PASS /dev/dmamem_fault: mmap and read()/write() see the same pages
#   done: all checks passed
sudo rmmod dmamem_fault          # dmamem_fault: 2 page faults served
sudo rmmod dmamem_demo

sudo insmod linux-device-drivers/08-dma-and-mmap/kernel/dmamem_stream.ko
#   dmamem_stream: INFO no DMA_MEMCPY channel (-517): device transfer skipped
#   dmamem_stream: INFO single: src phys 0x... -> dma 0x... (identity)
#   dmamem_stream: PASS single: map/unmap TO/FROM_DEVICE (no DMA_MEMCPY channel: API only)
#   dmamem_stream: PASS sync: map, sync_for_cpu, CPU edit, sync_for_device, unmap: data intact
#   dmamem_stream: INFO sg: segment 0: dma 0x... len 4096      (x4 without an IOMMU)
#   dmamem_stream: PASS sg: 4 pages mapped as 4 DMA segment(s)
#   dmamem_stream: PASS pool: 16 blocks of 64 bytes, every dma address 64-byte aligned
#   dmamem_stream: done: all checks passed
sudo rmmod dmamem_stream
# boot the VM with swiotlb=force and load dmamem_stream again: "translated/bounced"
```

`-517` is `-EPROBE_DEFER` (no dmaengine provider at all); a PC with an
Intel I/OAT or AMD PTDMA engine may find a real channel instead.

**BeagleBone AI-64** (K3 UDMA provides `DMA_MEMCPY`; NAVSS is
`dma-coherent`, so the buffer is cached memory):

```sh
sudo insmod dmamem_demo.ko buf_kb=1024
#   dmamem-demo dmamem-demo: 1024 KiB coherent buffer, dma 0x00000000..., mapped for 31150000.dma-controller, copy via dmaNchanM
#   dmamem-demo dmamem-demo: PASS selftest: 524288 bytes copied by DMA engine in ... ns
sudo ./dmamem_test
#   /dev/dmamem: 1048576 bytes, DMA channel: dmaNchanM
#   PASS copy: 524288 bytes by the DMA engine in ... us, verified through the mapping
sudo insmod dmamem_stream.ko len=65536
#   dmamem_stream: PASS single: dmaNchanM copied 65536 bytes src -> dst through streaming mappings (ret 0)
#   ...
#   dmamem_stream: done: all checks passed
cat /sys/class/dma/dma*chan*/in_use     # the channel shows as in use while dmamem_demo is loaded
```

Compare `use_dma=0` against `use_dma=1` with a large `buf_kb` to see the
engine's bandwidth versus the CPU's.

**Bug module** (VM or board, ideally with `CONFIG_DMA_API_DEBUG=y`):

```sh
make -C linux-device-drivers/08-dma-and-mmap/kernel BUGS=1
sudo insmod dmamem_bug.ko bug=1; sudo rmmod dmamem_bug
#   no engine:  "bug 1: no DMA_MEMCPY channel, nothing observed the stale data — the bug is still there"
#   board:      "bug 1: device copied 0x55 ..." (UDMA is coherent: the bug hides here too)
#   board booted with swiotlb=force: "device copied 0xaa" = STALE, deterministic
sudo insmod dmamem_bug.ko bug=3; sudo rmmod dmamem_bug
#   DMA-API: dmamem-demo ...: device driver frees DMA memory with different direction [...]
```

---

## 9. Exercises

**Easy**
1. Load `dmamem_demo` with `buf_kb=4096` and `use_dma=0`, then `use_dma=1`
   on the board. Compute MB/s for both from the `nsec` the ioctl returns.
2. In `dmamem_stream`, which test prints different DMA and physical
   addresses when you boot with `swiotlb=force`? Why that one?
3. Map `/dev/dmamem_fault` 16 pages but touch only 3. How many faults are
   reported at `rmmod` for `mode=0` and `mode=1`?

**Medium**
4. Add `DMAMEM_IOC_FILL`: the engine fills the buffer with a byte
   (`dmaengine_prep_dma_memset` if the channel has `DMA_MEMSET`, else CPU).
   Check the capability with `dma_has_cap()`.
5. Replace `dma_alloc_coherent` in `dmamem_demo` by a buffer from
   `alloc_pages()` that is mapped for the device with `dma_map_page()` per
   copy, and `mmap`ed with `remap_pfn_range`. Which syncs does the copy
   ioctl need now, in which direction, and when?
6. Make `dmamem_demo` asynchronous: the ioctl only submits; `poll()`
   reports completion (a wait queue woken from the DMA callback, lesson 03).
7. Write a UIO overlay for an unused TDA4VM timer block and blink its
   output from user space through `/dev/uio0`. Why is this a bad idea for
   anything that needs its interrupt at low latency?

**Hard**
8. Export the `dmamem_demo` buffer as a **DMA-BUF** (`dma_buf_export` with
   `map_dma_buf`, `unmap_dma_buf`, `mmap`, `release` ops) and return the
   fd from an ioctl. Import it in `dmamem_stream`-style test code with
   `dma_buf_attach`/`dma_buf_map_attachment` and copy into it with UDMA.
9. Pin a user buffer (`pin_user_pages_fast`, `sg_alloc_table_from_pages`,
   `dma_map_sgtable`) and let the engine copy into **user memory** directly.
   Handle partial pins and unpin with `unpin_user_pages_dirty_lock`.
10. Build a QEMU arm64 VM kernel with `CONFIG_DMA_API_DEBUG=y`, run all
    four bugs, and explain each report line, including the stack trace's
    "mapped at" part.

---

## 10. Remember forever

```
Device gets dma_addr_t (from the DMA API), never a pointer/virt_to_phys. Set the mask first:
    dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32|48|64)).
Coherent: cpu = dma_alloc_coherent(dev, sz, &dma, GFP_KERNEL); both sides any time; small -> dma_pool.
Streaming: h = dma_map_single(dev, buf, len, dir); CHECK dma_mapping_error; DEVICE OWNS until
    dma_unmap_single (same dev/len/dir). Peek: sync_for_cpu ... sync_for_device.
Non-coherent ARM: map TO_DEVICE = clean, FROM_DEVICE = invalidate (+ again at unmap). dma-coherent in DT = no-op.
dma_map_sg returns #segments (walk with it), unmap with original nents. Stack/vmalloc: not DMA-able.
dmaengine: request_chan(_by_mask) -> prep_* -> callback -> submit -> issue_pending -> wait; terminate_sync.
    Map for dmaengine_get_dma_device(chan). TDA4VM provider: K3 UDMA.
mmap: check size+pgoff, require VM_SHARED, vm_flags_set(DONTEXPAND|DONTDUMP);
    contiguous -> remap_pfn_range; pages -> vm_insert_page / .fault; DMA buffer -> dma_mmap_coherent;
    MMIO -> pgprot_noncached / writecombine. Big contiguous frames -> CMA (cma=).
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Name the four kinds of address in this lesson and who uses each. Why can
   a `dma_addr_t` differ from the physical address?
2. Draw what goes wrong on a non-coherent ARM device if the CPU writes a
   buffer and the device reads it without a cache clean. And the reverse
   direction?
3. What does `dma_set_mask_and_coherent()` promise, and what does the DMA
   layer do with it?
4. When do you choose a coherent allocation, and when a streaming mapping?
   What is the CPU-side cost of each on a non-coherent device?
5. State the ownership rules of a streaming mapping. Which calls move
   ownership, and in which direction?
6. Why must `DMA_FROM_DEVICE` invalidate both at map and at unmap time?
7. What can `CONFIG_DMA_API_DEBUG` detect, and what can it not? How do you
   find the rest?
8. Why does a 1080p camera pipeline on the AI-64 need CMA, and how do you
   size it?
9. List the dmaengine calls for one memcpy, in order. Why is
   `dmaengine_terminate_sync()` needed on timeout?
10. Which device must a buffer be mapped for when the DMA controller (not
    the peripheral) does the transfer?
11. Compare `remap_pfn_range`, `vm_insert_page`, a `.fault` handler and
    `dma_mmap_coherent`: what memory does each fit?
12. What does DMA-BUF add over "export an mmap-able buffer from my driver"?
    What are fences for?
