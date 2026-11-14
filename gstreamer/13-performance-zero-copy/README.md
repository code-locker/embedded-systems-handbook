# 13 — Performance and zero copy

> A 1080p30 NV12 stream is 93 MB/s. Every `memcpy` of it costs memory
> bandwidth the A72 cores on the TDA4VM do not have to spare. The fastest
> code is the copy you never make.

## 1. Why this exists

On a PC a careless pipeline still runs. On an embedded SoC the same pipeline
drops frames, burns 100% of a core, and adds 200 ms of latency. Almost every
case comes down to a few causes:

1. **Copies**: bytes moved by the CPU between buffers that could have been shared.
2. **Conversions**: `videoconvert` / `videoscale` doing per-pixel work on the
   CPU that a hardware block (or simply negotiating the right format) avoids.
3. **Allocation churn**: allocating and freeing big frames every frame
   instead of recycling them from a **buffer pool**.
4. **Waiting**: a sink synchronising to the clock (`sync=true`) when you want
   throughput, or queues growing until latency explodes (use **leaky** queues
   for live data).

This lesson shows exactly what a GStreamer buffer is, when it gets copied,
how pools and the allocation query avoid allocation, how `dmabuf` lets
hardware blocks share frames with zero CPU copies, and how to measure.

## 2. Mental model

**Analogy: a warehouse with labelled pallets.**

- The **GstMemory** is the pallet with the goods (pixels). Heavy. Moving it is
  expensive.
- The **GstBuffer** is the shipping label stapled to it: timestamps, flags,
  metadata. Cheap paper. You can photocopy labels all day.
- Two labels can point at the same pallet (`gst_buffer_copy`). Someone who
  wants to **paint** the goods must first get their own pallet
  (copy-on-write) — unless they are the only one holding a label.
- The **buffer pool** is a pallet rental yard: you rent a pallet, ship it,
  and when the last customer is done it comes back to the yard instead of
  being burned.
- **dmabuf** is putting the pallet on a forklift lane that the ISP, the scaler,
  the encoder and the display can all reach. Nobody unloads it by hand.

```
   GstBuffer (refcount, pts, dts, duration, offset, flags, metas...)
     │   metas: GstVideoMeta (stride[], offset[] per plane), crop, ...
     ├── GstMemory #0 ──► GstAllocator "SystemMemory"  (malloc)     bytes ....
     ├── GstMemory #1 ──► GstAllocator "dmabuf"        (fd = 23)    bytes on CMA/DDR
     └── ... up to 16
   GstMemory = { allocator, refcount, lock (exclusive/shared), maxsize, offset, size }
```

## 3. How it works

### 3.1 Buffer anatomy (program `01`)

| Operation | What is copied | Cost |
|-----------|----------------|------|
| `gst_buffer_ref(b)` | nothing, refcount++ | O(1) |
| `gst_buffer_copy(b)` | the header + metas; memories are **shared** (refcount++) | O(#memories) |
| `gst_buffer_make_writable(b)` | if refcount > 1: header copy (as above); else nothing | O(1) |
| `gst_buffer_map(b, WRITE)` on **shared** memory | the **bytes** (copy-on-write) | O(size) |
| `gst_buffer_map(b, READ)` on a buffer with **2+ memories** | merges them into one new block = **copy** | O(size) |
| `gst_buffer_copy_region(b, MEMORY, off, len)` | nothing; a sub-memory view with offset | O(1) |
| `gst_buffer_copy_deep(b)` | everything | O(size) |

`01` proves each line with pointer comparisons. The surprises people hit:
mapping a multi-memory buffer as one copies it, and a `WRITE` map silently
copies when the memory is shared (e.g. after a `tee`).

```
   tee
    ├──► queue ─► overlay (wants to WRITE)  ── buffer is shared with branch 2 ──► COPY
    └──► queue ─► encoder (reads)
   Rule: put the writer on its own buffer (or accept the copy knowingly).
```

**Writability is two-level:** the *buffer* is writable when its refcount is 1
(`gst_buffer_is_writable`); the *memory* is writable when only one buffer
uses it and it is not read-only-flagged (`gst_memory_is_writable`).

**Mapping video correctly:** use `gst_video_frame_map(&frame, &vinfo, buf, GST_MAP_READ)`.
It reads the `GstVideoMeta` (if present) to get the real stride and plane
offsets, and maps per plane without merging. Assuming `stride == width` is the
classic bug when hardware pads rows to 64 or 128 bytes.

### 3.2 Buffer pools and the allocation query (program `02`)

```
          (1) CAPS event: "I will send NV12 1920x1080"
   src ──────────────────────────────────────────────────────────► sink
          (2) ALLOCATION query: "how should I allocate?"
   src ──────────────────────────────────────────────────────────► sink
          (3) answer: "use MY pool (dmabuf, 4 buffers, 64-byte aligned),
                        and I support GstVideoMeta"
   src ◄────────────────────────────────────────────────────────── sink
          (4) src acquires buffers from that pool, fills them, pushes;
              sink releases => buffer returns to the pool
```

- The **downstream** element (often the sink or a hardware encoder) proposes
  a pool because it knows what memory it can use directly (display plane,
  encoder input, DMA-capable). If upstream writes into *that* memory, there is
  no copy at the boundary.
- `GST_BUFFER_POOL_OPTION_VIDEO_META` in the answer means "I understand
  strides/offsets", so upstream can use padded hardware layouts without
  repacking. If the downstream does NOT support video meta, upstream must
  produce tightly packed frames — often a copy.
- `min`/`max` buffers: if every buffer is held downstream (queued, displayed,
  referenced by an encoder), `acquire` blocks — back-pressure. A sink that
  holds too many buffers freezes a hardware capture pipeline: increase the
  pool size or reduce queue depths.

`02` shows a real pipeline pushing 100 frames of 450 KiB using only **2**
distinct memory blocks.

### 3.3 Wrapping external memory (program `03`)

```c
buf = gst_buffer_new_wrapped_full(flags, data, maxsize, offset, size,
                                  user_data, notify);
```

Your memory enters the pipeline with zero copies; `notify(user_data)` runs when
the last reference anywhere is dropped (possibly in another thread, possibly
much later if a queue holds it). That callback is your "slot is free again"
signal — the same pattern a V4L2 capture ring buffer uses. Flag
`GST_MEMORY_FLAG_READONLY` if downstream must never write into it (then
writers copy).

### 3.4 dmabuf: zero copy between hardware blocks

A **dmabuf** is a Linux file descriptor that names a buffer in DMA-able memory
(typically CMA on the TDA4VM). Any driver that imports it (V4L2, DRM/KMS,
the TI OVX/VPAC drivers, GPU via EGL) can read/write it directly.

```
   CSI camera ─► ISP (VPAC VISS) ─► scaler (VPAC MSC) ─► HW encoder ─► RTP
       │              │                   │                    │
       └──── one dmabuf fd per frame, passed by reference ─────┘
             CPU touches: 0 bytes of pixels       CPU copies if ANY link
                                                  falls back to sysmem
```

GStreamer pieces:

- `GstDmaBufAllocator` / `gst_is_dmabuf_memory(mem)` / `gst_dmabuf_memory_get_fd(mem)`.
- Caps feature `video/x-raw(memory:DMABuf)` on newer stacks (with
  `format=DMA_DRM` and `drm-format=NV12:...` in GStreamer ≥ 1.24) tells
  negotiation that only dmabuf-capable elements may link there.
- `v4l2src io-mode=dmabuf` (driver exports dmabufs) and on V4L2 M2M elements
  `output-io-mode=dmabuf-import` (encoder imports upstream's dmabuf). Check
  the exact property names and values with `gst-inspect-1.0` on your board.
- If an element in the middle only understands system memory (e.g.
  `videoconvert`), the chain maps the dmabuf on the CPU: cache maintenance +
  CPU access to uncached memory — *slower than a plain memcpy*. One CPU element
  in a hardware chain destroys the whole benefit.

### 3.5 Avoid videoconvert (and videoscale) — negotiate instead

`videoconvert` is passthrough (free) when input and output caps match, and a
full per-pixel CPU loop when they don't. Program `04` measures it: on the PC,
I420→RGBx for 720p is ~0.8 ms/frame with SIMD (ORC); on an A72 with 1080p and
`SAN=0` expect several ms per frame per core — a third of your 33 ms budget.

How to avoid it:

1. Ask the source for what the consumer wants: `v4l2src ! video/x-raw,format=NV12,...`.
   Look at `v4l2-ctl --list-formats-ext` and the encoder's sink template.
2. Use a hardware converter/scaler where one exists (on TDA4VM: TI's
   `tiovxdlcolorconvert` / `tiovxmultiscaler`, TI SDK image only).
3. Keep `videoconvert` in a pipeline only as a **safety net** and verify with
   `gst-launch-1.0 -v` that it is passthrough (same caps on both pads).
4. Never leave caps unconstrained: `videotestsrc ! fakesink` negotiates
   `A444_16LE` (see lesson 12) — 8 bytes per pixel.

### 3.6 sync=false, leaky queues, QoS

| Knob | Effect | When |
|------|--------|------|
| sink `sync=false` | renders/consumes as soon as data arrives, ignores the clock | file processing, benchmarks, live pipelines where you want minimum latency and the source already paces |
| sink `sync=true` (default) | waits for each buffer's running time | playback at real speed, A/V sync |
| `queue leaky=downstream max-size-buffers=1` | drops the **oldest** buffer when full | live preview/AI: always process the newest frame |
| `queue leaky=upstream` | drops the **new** buffer when full | keep a contiguous segment |
| `max-size-time=0 max-size-bytes=0` | only the buffers limit applies | predictable latency |
| `qos=true` (sinks, default for video sinks) | upstream skips late frames | smooth playback under load |
| `appsink drop=true max-buffers=1` | appsink keeps only the newest | AI inference consumer |
| `videorate drop-only=true` / caps framerate | reduce frame rate early | less work for every downstream element |

### 3.7 Measuring

- Tracers from lesson 12: `latency(flags=element)`, `stats`, `rusage`
  (per-thread CPU). GstShark `proctime`/`interlatency` per element.
- `fpsdisplaysink video-sink=fakesink text-overlay=false -v` prints current/
  average fps (`fpsdisplaysink` is in **gst-plugins-bad**, `gstreamer1.0-plugins-bad`).
- `top -H` / `htop` with thread names: GStreamer threads are named after
  their element pads.
- `perf top -p $(pidof myapp)` — if `videoconvert`/`orc`/`memcpy` is on top,
  you have found your copy.
- Always measure with `make OPT=1 SAN=0`: ASan makes pixel loops 2–5× slower.

## 4. Complexity table

| Operation | CPU cost per 1080p NV12 frame (3.1 MB) | Copies | Notes |
|-----------|-----------------------------------------|--------|-------|
| push a buffer to the next element | O(1) | 0 | function call + refcount |
| `tee` to N branches | O(N) | 0 | N refs to the same buffer |
| `gst_buffer_copy` | O(1) | 0 bytes | header + metas |
| map WRITE on shared memory | O(size) | 1 | copy-on-write |
| `videoconvert` passthrough | O(1) | 0 | caps equal |
| `videoconvert` NV12→RGB | O(pixels) | 1 + math | ms on A72 |
| `videoscale` 1080p→720p | O(output pixels × taps) | 1 + math | |
| pool acquire/release | O(1) | 0 | no malloc |
| `gst_buffer_new_allocate` per frame | malloc(3 MB) + page faults | 0 | churn; CMA allocation is far slower |
| wrap external memory | O(1) | 0 | must manage lifetime via notify |
| dmabuf hand-off between HW blocks | O(1) | 0 | fd passing; CPU never touches pixels |
| CPU map of a dmabuf | cache sync O(size) | 0–1 | slow on uncached memory |
| sink `sync=true` | adds waiting, not CPU | 0 | throughput capped to framerate |

## 5. C vs C++

The API is C; the ideas carry over:

- **Map/unmap is a scope**: in C++ wrap `GstMapInfo` in a small RAII struct
  whose destructor calls `gst_buffer_unmap` (an early `return` with a mapped
  buffer leaks a lock and, for dmabuf, a CPU access window).
- Wrapped memory with a C++ owner: pass a heap-allocated
  `std::shared_ptr<Frame>*` as `user_data` and `delete` it in the notify —
  the frame lives exactly as long as GStreamer needs it.
- `std::vector<uint8_t>` → zero-copy: `gst_buffer_new_wrapped_full(0, v->data(), v->size(), 0, v->size(), v, [](gpointer p){ delete static_cast<std::vector<uint8_t>*>(p); })`.
- `gst_buffer_ref/unref` map naturally to a `unique_ptr<GstBuffer, Deleter>`
  (lesson 09); `make_writable` *consumes* its argument — call `.release()`
  first and re-wrap the result.

## 6. Where it is used in the real world

- **BeagleBone AI-64 / TDA4VM Edge AI demos:** camera → VPAC ISP →
  MSC multiscaler (several resolutions at once) → TIDL inference on C7x/MMA
  and → the hardware H.264 encoder (V4L2 M2M), all exchanging dmabufs. The A72 cores mostly just
  run the GStreamer scheduling and post-processing.
- **Every hardware video player** (Raspberry Pi, i.MX, Rockchip, Jetson): a
  decoder exports dmabufs and `kmssink`/`waylandsink` displays them without a
  single CPU pixel copy.
- **Video analytics** (DeepStream, TI Edge AI, OpenVINO): `appsink drop=true
  max-buffers=1` + leaky queues so inference always runs on the newest frame.
- **Browsers (WebKit/Chromium GStreamer backends)**: buffer pools of GL
  textures to avoid upload copies.
- **The Linux kernel**: dmabuf (`drivers/dma-buf/`) is the kernel-side
  object every one of these uses.

## 7. Common mistakes and pitfalls

1. **Assuming `stride == width × bpp`.** Use `GstVideoInfo`/`GstVideoMeta`
   and `gst_video_frame_map`. Hardware pads rows.
2. **Writing into a buffer without `make_writable`** → you corrupt the other
   tee branch (or the source's pool buffer). Always make writable first.
3. **Holding pool buffers** (an appsink sample you never unref, a queue with
   200 buffers) → the source blocks in `acquire` → the pipeline freezes.
4. **A CPU element in the middle of a dmabuf chain** (`videoconvert`,
   `videoflip`) → caches flushed every frame, slower than having no
   hardware at all.
5. **`sync=false` on a playback sink** → video plays as fast as the CPU can
   decode; audio and video desynchronise.
6. **Unbounded queues on live sources** → latency grows forever. Use
   `leaky=downstream` with a small `max-size-buffers`.
7. **Benchmarking a debug/ASan build.** Use `make OPT=1 SAN=0`.
8. **Freeing wrapped memory before the notify** → use-after-free in a
   streaming thread minutes later (ASan in this repo will catch it).

## 8. Files in this lesson

| File | What it shows | Run |
|------|---------------|-----|
| [01_buffer_memory_anatomy.c](01_buffer_memory_anatomy.c) | buffer = header + memories; multi-memory map merges (copies); `gst_buffer_copy` shares bytes; WRITE map copy-on-write; `make_writable`; zero-copy regions | `make run P=gstreamer/13-performance-zero-copy/01_buffer_memory_anatomy.c` |
| [02_buffer_pool.c](02_buffer_pool.c) | `GstVideoBufferPool` by hand (config, video meta, recycle, exhaustion), then 100 pipeline frames using 2 memory blocks | `make run P=gstreamer/13-performance-zero-copy/02_buffer_pool.c` |
| [03_zero_copy_wrap.c](03_zero_copy_wrap.c) | `gst_buffer_new_wrapped_full` through appsrc→appsink: same pointer at the sink, destroy-notify for every frame | `make run P=gstreamer/13-performance-zero-copy/03_zero_copy_wrap.c` |
| [04_throughput_sync.c](04_throughput_sync.c) | timing: `sync=true` vs `false`; `videoconvert` passthrough vs I420→RGBx at 720p | `make run P=gstreamer/13-performance-zero-copy/04_throughput_sync.c` (realistic: `make OPT=1 SAN=0 run P=...`) |

Build requirements: `sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev`.

## 9. Exercises

1. *(easy)* In `01`, add a third memory and check that `copy_region`
   spanning two memories returns a buffer with 2 memories (no merge).
2. *(easy)* In `02`, set `max=0` and acquire 10 buffers: how many does the
   pool create? Then set `min=4` and observe allocation at `set_active`.
3. *(medium)* In `03`, add a `tee` (like `--display`) and print *when* each
   notify fires relative to `pull_sample` — explain the delay.
4. *(medium)* Write a pad probe that prints `gst_buffer_n_memory`,
   the allocator name (`mem->allocator->mem_type`) and whether a video meta
   exists, for `videotestsrc ! videoconvert ! fakesink` and for
   `v4l2src` on a USB camera. *Hint:* `gst_buffer_peek_memory`.
5. *(medium)* Rerun `04` with `make OPT=1 SAN=0` and with
   `videoconvert n-threads=4`. How does per-frame time scale?
6. *(hard)* Build an appsrc producer that uses its own `GstBufferPool`
   (acquire, fill, push) instead of allocating each frame, and show with the
   probe from (4) that only `min` distinct memories appear.
7. *(hard, board)* On the BeagleBone AI-64, compare CPU (`rusage` tracer) for
   `v4l2src ! videoconvert ! fakesink` vs the same with caps matching the
   camera's native format. Then try `io-mode=dmabuf` on `v4l2src`.

## 10. Remember forever

```
GstBuffer = cheap header (pts/flags/metas) + up to 16 GstMemory (the bytes)
copy() shares bytes | WRITE map of shared memory = copy | map of N memories = merge copy
make_writable before writing; gst_video_frame_map for strides (never stride==width)
pools: ALLOCATION query lets downstream propose memory; release -> back to pool
held buffers => acquire blocks => pipeline freezes
wrapped_full(data, ..., user_data, notify): zero copy, free only in notify
dmabuf = fd shared by HW blocks; one CPU element in the chain kills it
avoid videoconvert by negotiating native formats; live: leaky queue + sync=false
measure with OPT=1 SAN=0, tracers, top -H, perf
```

Review it with the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

## 11. Self-test questions

1. What does `gst_buffer_copy` copy, and what does it share?
2. When does mapping a buffer for READ cost a full copy?
3. Two tee branches; one writes an overlay. Where does a copy happen and why?
4. Who proposes the buffer pool in the allocation query and why that side?
5. Why can a sink that keeps references freeze a capture pipeline?
6. What is a dmabuf, and why does a `videoconvert` in a dmabuf chain hurt more
   than it would in a plain sysmem pipeline?
7. When is `sync=false` right, and when is it a bug?
8. `leaky=downstream` vs `leaky=upstream`: which frames are dropped, and which
   one do you want for live AI inference?
9. Why must you not free wrapped memory right after `gst_app_src_push_buffer`
   returns?
