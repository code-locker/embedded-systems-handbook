# 00 — GStreamer concepts: the whole map before the first line of code

> Read this once fully, then come back to it whenever a later lesson uses a
> word you cannot explain. Every other lesson zooms into one box of the map
> drawn here.

## 1. Why this exists

A camera gives you raw frames. You want them scaled, converted, encoded to
H.264, sent over the network, and recorded to disk, all at once, at 30 fps,
without dropping frames, with audio in sync, on a small ARM board. Written by
hand, that is threads, ring buffers, format conversion, timestamps, A/V sync,
back-pressure and error handling: months of work, and full of bugs.

**GStreamer** is a framework that gives you all of that as **reusable blocks
(elements)** that you plug together into a **pipeline**. You describe *what*
should happen (`camera ! scale ! encode ! network`); GStreamer handles *how*:
the threads, the buffering, agreeing on formats, timing and synchronisation.
It is the standard media framework on embedded Linux: TI's Edge AI SDK for
the TDA4VM (BeagleBone AI-64), NVIDIA DeepStream, NXP i.MX, Rockchip, and
desktop Linux players are all built on it.

## 2. Mental model

**Analogy: a water-treatment plant built from pipe fittings.**

| Plumbing                       | GStreamer                                 |
|--------------------------------|-------------------------------------------|
| A machine (pump, filter, tap)  | **element** (`videotestsrc`, `jpegenc`)   |
| The pipe sockets on a machine  | **pads** (`src` = outlet, `sink` = inlet) |
| The thread size of a socket    | **caps** (`video/x-raw,format=NV12,...`)  |
| The water flowing              | **buffers** (one frame / audio chunk)     |
| Notes floating with the water  | **events** (EOS, SEGMENT, CAPS, FLUSH)    |
| Shouting to a machine upstream | **queries** ("what can you accept?")      |
| The control-room intercom      | the **bus** (messages to your app)        |
| A cabinet holding machines     | **bin**; the whole plant = **pipeline**   |
| The plant's master clock       | the **clock**                             |
| The on/off/standby switch      | **states** NULL / READY / PAUSED / PLAYING|

Water flows **downstream** (source → sink). You only connect two sockets if
their thread sizes fit (**caps negotiation**). The plant runs by itself; the
operator (your application) only listens to the intercom (bus) and flips
switches (states).

```
                 PIPELINE  (a bin that owns the clock and the bus)
 ┌──────────────────────────────────────────────────────────────────────┐
 │  ┌────────────┐      ┌──────────────┐      ┌────────────┐            │
 │  │videotestsrc│      │ videoconvert │      │ autovideo  │            │
 │  │        src ├─────►┤ sink     src ├─────►┤ sink  sink │            │
 │  └────────────┘ pad  └──────────────┘  pad └────────────┘            │
 │      buffers + downstream events ───────────────────────►            │
 │      ◄─────────────────────── upstream events (SEEK, QOS)            │
 │      ◄──────────────────────► queries (CAPS, POSITION, LATENCY)      │
 └──────────────┬───────────────────────────────────────────────────────┘
                │ messages (EOS, ERROR, STATE_CHANGED, ...)
                ▼
            ┌──────┐  gst_bus_timed_pop / bus watch
            │ BUS  ├──────────────────────────────►  YOUR APPLICATION
            └──────┘                                 (main thread)
```

## 3. How it works

### 3.1 Architecture: core, plugins, applications

```
 ┌──────────────────────── your application (C / C++ / Python / Rust) ──┐
 │                  gst_parse_launch, gst_element_factory_make, bus ...  │
 ├───────────────────────────────────────────────────────────────────────┤
 │ libgstreamer-1.0 (CORE): GstElement, GstPad, GstBin, GstBus, GstClock │
 │   GstBuffer, GstCaps, GstEvent, scheduling, registry. NO media code!  │
 │ helper libs: gstbase (BaseSrc/BaseTransform/BaseSink), gstapp,        │
 │   gstvideo, gstaudio, gstrtp ...                                      │
 ├───────────────────────────────────────────────────────────────────────┤
 │ PLUGINS (.so in /usr/lib/<triplet>/gstreamer-1.0/), loaded on demand: │
 │   plugins-base  : well-maintained, "must have" (videotestsrc,         │
 │                   videoconvert, appsrc/appsink, playbin, ogg, theora)  │
 │   plugins-good  : good quality, good licence (v4l2, jpeg, rtp, udp,   │
 │                   matroska, vpx, autodetect)                           │
 │   plugins-bad   : not yet up to standard (h264parse, kmssink, srt)     │
 │   plugins-ugly  : good code, licence/patent issues (x264enc)           │
 │   libav         : FFmpeg decoders/encoders (avdec_h264)                │
 │   vendor        : TI tiovx*/tidl*, NVIDIA nv*, NXP imx* ...            │
 └───────────────────────────────────────────────────────────────────────┘
```

The core knows nothing about video or audio: everything media-specific is a
plugin. `gst_init()` scans the plugin directories once and caches the result
in the **registry** (`~/.cache/gstreamer-1.0/registry.*.bin`). A plugin
provides one or more **element factories**; `gst_element_factory_make("jpegenc",
...)` asks the registry for the factory and creates an **element**.
`gst-inspect-1.0 <name>` prints everything the registry knows about it
(program `02_element_anatomy.c` is a mini version).

### 3.2 GObject in 5 minutes (the #1 source of leaks)

GStreamer is written in C on top of **GObject**, GLib's object system. You
need four ideas:

**(a) Reference counting.** Every object has a counter. `gst_object_ref()`
+1, `gst_object_unref()` −1, at 0 it is freed. Rule: *every ref you take,
you give back exactly once*.

**(b) Transfer annotations** in the docs tell you whether a function gives
you a ref:

| Annotation         | Meaning                              | Examples                                   |
|--------------------|--------------------------------------|--------------------------------------------|
| `transfer full`    | you get a ref, **you must unref**    | `gst_element_get_bus`, `gst_element_get_static_pad`, `gst_bin_get_by_name`, `gst_pad_get_current_caps`, `gst_bus_pop`, `gst_app_sink_pull_sample` |
| `transfer none`    | borrowed, **do not unref**           | `gst_sample_get_buffer`, `gst_caps_get_structure`, `GST_MESSAGE_SRC` |
| `transfer floating`| new object with a *floating* ref     | `gst_element_factory_make`, `gst_bin_new`, `gst_pipeline_new` |

**(c) Floating references.** A new element starts with refcount 1 marked
*floating* = "nobody has claimed me yet". The first container that takes it
(`gst_bin_add`, `gst_element_add_pad`) **sinks** the floating ref: it takes
ownership *without* adding a ref. That is why this does not leak:

```c
gst_bin_add(GST_BIN(pipeline), gst_element_factory_make("fakesink", NULL));
```

```
 factory_make ─► [rc=1 floating]  ─ gst_bin_add ─► [rc=1 owned by bin]
                                                     you hold a BORROWED ptr
 pipeline unref ─► bin unrefs children ─► [rc=0] freed
```

After `gst_bin_add`, your pointer is borrowed. If you want the element to
outlive the bin, `gst_object_ref()` it first. If you create an element and
never add it to a bin, call `gst_object_ref_sink()` + `gst_object_unref()` or
just `gst_object_unref()` (unref on a floating object also frees it).

**(d) Properties and signals.** Elements are configured with typed
**properties** (`g_object_set(src, "num-buffers", 60, NULL)`; `NULL` ends the
list, forgetting it is a crash). They talk back with **signals**
(`g_signal_connect(demux, "pad-added", G_CALLBACK(cb), data)`). Signal
callbacks run in **whatever thread emitted them**, often a streaming thread,
not your main thread. Strings you `g_object_get` are copies: `g_free` them.

**Mini-objects** (`GstBuffer`, `GstCaps`, `GstEvent`, `GstMessage`,
`GstQuery`, `GstSample`) are lighter: refcounted but no floating refs, no
signals. They are **copy-on-write**: writable only if refcount == 1, so
`gst_buffer_make_writable()` copies when shared. Program
`01_gobject_refcount.c` proves each rule with asserted refcounts.

### 3.3 Elements, pads, caps

An **element** has **pads**: `sink` pads receive data, `src` pads produce it.
Each pad is created from a **pad template** with an *availability*:

```
  ALWAYS     exists from creation           videoconvert: sink, src
  SOMETIMES  appears when data says so      matroskademux: video_0, audio_0
             (you connect in "pad-added")   decodebin: src_0, src_1
  REQUEST    created when you ask for it    tee: src_%u, funnel/mux: sink_%u
```

**Caps** (capabilities) describe the data a pad can carry:

```
  video/x-raw, format=(string)NV12, width=(int)1920, height=(int)1080,
               framerate=(fraction)30/1
  └─ media type ┘└──────────── fields (may be lists / ranges) ─────────┘
```

Template caps are broad ("any size, many formats"). Before data flows, linked
pads **negotiate** one *fixed* set: the source proposes, downstream answers
with what it can accept, they **intersect**, and the result is sent as a
`CAPS` event. If the intersection is empty you get the famous
`not-negotiated` error. Converters (`videoconvert`, `videoscale`,
`audioconvert`, `audioresample`) exist to make intersections non-empty.
Lesson 05 covers this in depth.

### 3.4 Bins and pipelines

A **bin** is an element that contains elements; it forwards state changes to
its children and collects their bus messages. A **pipeline** is the top-level
bin: it also chooses the **clock**, distributes **base time**, and owns the
**bus**. Bins nest, which is how `decodebin`, `playbin` and your own reusable
blocks are built.

### 3.5 States

```
   NULL ──► READY ──► PAUSED ──► PLAYING
    ▲  ◄──       ◄──        ◄──    │
    │ no resources   │ data flows   │ clock runs,
    │ allocated      │ up to sinks, │ sinks render
    │                │ sinks block  │
    │      device    │ = PREROLL    │
    │      opened    │ (1st frame   │
    │      (READY)   │  ready)      │
```

* **NULL**: just created, nothing allocated. You must return to NULL before
  the final unref.
* **READY**: resources opened (device, socket, file), no data.
* **PAUSED**: data flows until each sink holds one buffer and blocks:
  **preroll**. The pipeline is "armed": a video sink shows the first frame.
* **PLAYING**: the clock runs and sinks render buffers on time.

`gst_element_set_state()` returns `SUCCESS`, `FAILURE`, `NO_PREROLL` (live
sources: they cannot preroll, nothing exists before "now") or **`ASYNC`**: the
change continues in the background (preroll takes time). Wait with
`gst_element_get_state(e, &cur, &pend, timeout)` or for the
`ASYNC_DONE` / `STATE_CHANGED` bus messages. You can jump NULL → PLAYING;
GStreamer walks through the intermediate states for you.

### 3.6 Clocks, time and synchronisation

Three timelines, and confusing them is the root of most A/V sync bugs:

```
 clock time    (absolute, monotonic: e.g. 5 001 000 000 ns since boot)
     │ base_time = clock time when the pipeline went to PLAYING
     ▼
 running time  = clock_time − base_time   (stops while PAUSED)
     ▲
     │ the SEGMENT event maps buffer timestamps to running time
     │ (handles seeks, rate changes, looping)
 stream time / buffer PTS (position inside the media: 00:01:23.4)
```

* **PTS** (presentation timestamp): *when to show it*. **DTS** (decode
  timestamp): *when to decode it*; differs from PTS only with B-frames.
  **duration**: how long it lasts.
* A synchronising sink (`sync=true`: the default for display, audio and
  app sinks; `fakesink` alone defaults to `sync=false`) waits until
  `clock_time − base_time ≥ running_time(PTS) + latency` and then renders.
  `sync=false` renders immediately (good for tests and for files, wrong for
  live display).
* The pipeline picks the **clock** from the element that provides the best
  one (audio sinks provide one so audio never stutters; live sources like
  `v4l2src` can provide one), else the system monotonic clock.
* **Live sources** (camera, network) produce data in real time; the pipeline
  computes a **latency** (LATENCY query) and sinks add it to every wait.

Program `03_clock_and_time.c` measures that a sink renders each frame at
`base_time + PTS`.

### 3.7 Buffers and memory

```
 GstBuffer (refcounted mini-object)
 ├─ PTS, DTS, duration, offset, flags (DISCONT, DELTA_UNIT, ...)
 ├─ metas: GstVideoMeta (strides/offsets), crop, ROI, your own ...
 └─ 1..N GstMemory ──► the actual bytes (from an allocator: system RAM,
                       dmabuf fd, GPU/accelerator memory)
                    ▲
 GstBufferPool: recycles buffers of the same size → no malloc per frame
```

You read bytes with `gst_buffer_map(buf, &info, GST_MAP_READ)` …
`gst_buffer_unmap()`. On the TDA4VM, buffers can be **dmabuf** file
descriptors shared between camera, ISP, scaler, encoder and display without
copying a single byte (lesson 13 and 14).

### 3.8 Events and queries

**Events** travel *along* the pipeline, in order with the data:

| Event          | Direction  | Meaning                                         |
|----------------|------------|-------------------------------------------------|
| STREAM_START   | downstream | a new stream begins                             |
| CAPS           | downstream | "from now on the data has this format"          |
| SEGMENT        | downstream | how to map timestamps to running time           |
| EOS            | downstream | no more data; sinks post EOS on the bus         |
| FLUSH_START/STOP | both     | throw away everything in flight (used by seeks) |
| SEEK           | upstream   | "jump to position X / change rate"              |
| QOS            | upstream   | "I'm late by N ms": lets sources/decoders skip  |
| RECONFIGURE    | upstream   | "renegotiate caps please"                       |

**Queries** are questions answered synchronously: `CAPS` ("what can you
accept?"), `ACCEPT_CAPS`, `ALLOCATION` (who provides buffer pools),
`POSITION`, `DURATION`, `LATENCY`, `SEEKING`.

**Messages** are different: they go from elements to the **application**
through the **bus** (`EOS`, `ERROR`, `WARNING`, `STATE_CHANGED`,
`ASYNC_DONE`, `ELEMENT`, `BUFFERING`, `LATENCY`, `QOS`). Lesson 03.

```
 events:   element ──pad──► element ──pad──► element     (in the stream)
 queries:  element ◄─ask──► element                      (synchronous Q&A)
 messages: element ────────► bus ────────► application   (out of the stream)
```

### 3.9 Threads

GStreamer creates threads for you:

* Each **source** element (in push mode) runs a **streaming thread** that
  pushes buffers downstream through every element *by function call* until it
  hits a `queue` or the sink.
* Every **queue** (`queue`, `queue2`, `multiqueue`) starts a **new streaming
  thread** on its src side: that is how you get parallelism.
* Your **application thread** only receives bus messages; it must never do
  heavy work inside callbacks that run in the streaming thread (`pad-added`,
  `handoff`, pad probes, appsink `new-sample`).

```
 app thread:    main() ── bus pop / GMainLoop ────────────────────────
 thread 1:      [src] → [convert] → [queue sink]
 thread 2:                          [queue src] → [encoder] → [sink]
```

### 3.10 Push vs pull scheduling

Most pipelines **push**: the source's thread calls `gst_pad_push()`
downstream. Some elements **pull**: a demuxer can drive the pipeline and call
`gst_pad_pull_range()` on `filesrc` to read random offsets. This is chosen
automatically during activation; you rarely care, except to know that
"who owns the thread" decides where the work runs.

## 4. Complexity table

| Operation                          | Cost                               | Why |
|------------------------------------|------------------------------------|-----|
| `gst_init`                         | ms (first run: 100s of ms)         | scans plugins once, then reads the registry cache |
| `gst_element_factory_make`         | µs                                 | registry lookup + GObject construction; plugin `.so` loaded on first use |
| Linking two pads                   | O(caps size)                       | template caps compatibility check only |
| Caps negotiation                   | O(elements × caps fields)          | queries walk the pipeline before the first buffer |
| `gst_pad_push` (one buffer)        | 1 function call per element        | data passes by pointer; no copy unless an element needs writable data |
| Crossing a `queue`                 | 1 mutex + cond signal              | thread hand-off; adds latency up to queue size |
| `gst_buffer_map` READ              | O(1) for system memory             | may be O(size) for dmabuf/GPU memory (cache sync / download) |
| `gst_buffer_make_writable` shared  | O(size)                            | copy-on-write copies the bytes |
| `videoconvert` per frame           | O(width × height)                  | touches every pixel: the most common hidden CPU cost |
| Bus message                        | 1 alloc + queue push               | async: your app sees it later |

## 5. C vs C++

GStreamer is a C API. In C you manage every ref by hand or with GLib's
`g_autoptr(GstElement)` cleanup attribute. In C++ you wrap each type in
`std::unique_ptr<T, Deleter>` so refs are released automatically, even on
exceptions (lesson 09). One C++ gotcha: GStreamer flag enums combined with
`|` become `int`, and C++ will not convert `int` back to an enum, so you write
`static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR)`.
There is also the official C++ binding **gstreamermm** (less maintained) and
first-class Rust and Python bindings.

## 6. Where it is used in the real world

* **TI Edge AI SDK on TDA4VM / BeagleBone AI-64**: camera → ISP → scaler
  → TIDL deep-learning inference → overlay → display/encode are all
  GStreamer elements (`tiovx*`, `tidl*`), lesson 14.
* **NVIDIA DeepStream** (Jetson), **NXP i.MX** (`imxvideoconvert`), Rockchip
  (`mpph264enc`): every embedded SoC vendor ships GStreamer plugins for its
  hardware blocks.
* Desktop: GNOME Videos/Totem, Rhythmbox, Cheese, WebKitGTK media, PipeWire
  helpers; broadcast: OBS-like apps, Pexip, RidgeRun products; robotics: ROS
  camera drivers.
* IP cameras, drones, dash cams, video doorbells, in-car infotainment.

## 7. Common mistakes and pitfalls

1. **Forgetting to unref** what a `transfer full` getter returned (bus, pad,
   caps, element from `get_by_name`). Leaks accumulate per frame if done in
   callbacks.
2. **Unref'ing a borrowed pointer** (e.g. an element after `gst_bin_add`, a
   buffer from `gst_sample_get_buffer`): double free / crash later.
3. **Not setting state to NULL** before the last unref: threads still run
   against freed memory.
4. **Forgetting the `NULL` terminator** in `g_object_set(...)`.
5. **Doing heavy work in a streaming-thread callback**: stalls the pipeline.
6. **Expecting EOS from a live source**: a camera never ends; send EOS
   yourself with `gst_element_send_event(pipeline, gst_event_new_eos())`.
7. **Mixing up PTS and running time** when you timestamp buffers yourself.
8. **Ignoring the bus**: errors are posted as messages; if you never read the
   bus, your app just hangs silently.

## 8. Files in this lesson

| File | What it shows | Run |
|------|---------------|-----|
| [01_gobject_refcount.c](01_gobject_refcount.c) | floating refs, sinking, transfer full, properties, notify signal, copy-on-write, with asserted refcounts | `make run P=gstreamer/00-concepts/01_gobject_refcount.c` |
| [02_element_anatomy.c](02_element_anatomy.c) | a mini `gst-inspect`: factory metadata, rank, pad templates (ALWAYS/SOMETIMES/REQUEST), properties | `make run P=gstreamer/00-concepts/02_element_anatomy.c` |
| [03_clock_and_time.c](03_clock_and_time.c) | pipeline clock, base time, running time vs PTS, position query | `make run P=gstreamer/00-concepts/03_clock_and_time.c` |

Install the development files first: `sudo apt install libgstreamer1.0-dev
libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-good gstreamer1.0-tools`.

## 9. Exercises

1. *(easy)* Run `gst-inspect-1.0 videotestsrc` and find `pattern`,
   `num-buffers` and `is-live`. Compare with `./build/gstreamer/00-concepts/02_element_anatomy videotestsrc`.
2. *(easy)* In `01_gobject_refcount.c`, remove the `gst_object_unref(found)`
   line and add a final `CHECK(src_dead, ...)`: what happens and why?
3. *(medium)* Inspect `matroskademux`, `decodebin` and `funnel` with program 02.
   Predict each pad's availability before running.
4. *(medium)* In `03_clock_and_time.c`, set `sync=false`. Which check fails
   and why?
5. *(hard)* Add `is-live=true` to `videotestsrc` in program 03 and print the
   return value of `gst_element_set_state(..., GST_STATE_PAUSED)`. Explain
   `NO_PREROLL`.
6. *(hard)* Draw, from memory, the three timelines of section 3.6 and explain
   what happens to running time when you pause for 2 seconds.

## 10. Remember forever

```
Element = box; pads = sockets (src out, sink in); caps = socket shape.
Pads: ALWAYS / SOMETIMES (pad-added) / REQUEST (you ask, you release).
Pipeline = top bin: owns clock + bus. NULL→READY→PAUSED(preroll)→PLAYING.
Data down (buffers + events); questions = queries; app hears the bus.
Running time = clock − base_time; sink shows buffer when running ≥ PTS.
Refs: transfer full → you unref; floating → first bin sinks it.
Always set NULL before the last unref. Each queue = a new thread.
```

Review this card on the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

## 11. Self-test questions

1. What is the difference between an element, a factory and a plugin?
2. Why does `gst_bin_add(bin, gst_element_factory_make(...))` not leak?
3. Name the three pad availabilities and one element for each.
4. What does PAUSED do that READY does not? What is preroll?
5. Why does `gst_element_set_state` return `ASYNC`, and how do you wait for it?
6. Define clock time, base time, running time and PTS in one sentence each.
7. Which way does a SEEK event travel? An EOS event?
8. What is the difference between an event, a query and a message?
9. Where does a `pad-added` callback run, and what must you not do in it?
10. Why must GStreamer flag enums be cast in C++?
