# GStreamer course: from `gst-launch` to zero-copy pipelines on the TDA4VM

A hands-on course in **C and C++**, from the first pipeline to custom
elements, network streaming, profiling and the hardware-accelerated
pipelines of the **BeagleBone AI-64 (TI TDA4VM / J721E)**.

Every lesson has the same shape ([CONVENTIONS](../docs/CONVENTIONS.md)):
an analogy first, then the real mechanics with ASCII diagrams of pipelines,
pads and threads, real-world use, pitfalls, exercises and a **recall card**.
Every program is one standalone file that runs **headless**, finishes in a
few seconds, **checks its own results** and exits 0, so `make test-gst`
proves the whole course still works.

## The whole course on one page

```
 element = box     pad = socket (src out / sink in)     caps = socket shape
 pipeline = top bin: owns the CLOCK and the BUS

   ┌───────── pipeline ───────────────────────────────────────────────┐
   │ v4l2src ─► queue ─► videoconvert ─► tee ─┬► queue ─► appsink ──────┼─► your C code
   │  (thread 1)  │(thread 2)                 └► queue ─► encoder ─► udpsink
   └──────────────┴───────────────────────────────────────────────────┘
        │ messages (EOS, ERROR, STATE_CHANGED) ─► bus ─► main thread
 states: NULL → READY → PAUSED (preroll) → PLAYING
 time:   sink renders a buffer when  clock − base_time ≥ PTS (+ latency)
```

## Learning path

| # | Lesson | You will be able to... | Language |
|---|--------|------------------------|----------|
| 00 | [Concepts](00-concepts/) | explain elements, pads, caps, bins, states, clocks, buffers, events, queries, threads, and the **GObject refcount/floating-ref rules** | C |
| 01 | [gst-launch-1.0](01-gst-launch/) | prototype any pipeline in the shell, read `-v` caps, use `gst-inspect-1.0` / `gst-discoverer-1.0` | shell |
| 02 | [First pipeline](02-first-pipeline/) | write the 6-step skeleton, build and link by hand, understand ASYNC preroll and NO_PREROLL | C, C++ |
| 03 | [Bus and messages](03-bus-and-messages/) | run a `GMainLoop` bus watch, classify errors, send events, run queries, **seek and change rate** | C |
| 04 | [Dynamic pads](04-dynamic-pads/) | link sometimes pads in `pad-added`, use `decodebin` / `uridecodebin` / `playbin`, request and release pads | C |
| 05 | [Caps and negotiation](05-caps-and-negotiation/) | build, intersect and fixate caps; read what was negotiated; fix `not-negotiated` | C |
| 06 | [appsrc / appsink](06-appsrc-appsink/) | push your own frames in, pull frames out into your code (pull and callbacks, timestamps, back-pressure) | C, C++ |
| 07 | [Pad probes](07-pad-probes/) | inspect, drop and modify buffers, block a pad to **swap elements while PLAYING** | C |
| 08 | [Queues, threads, tee](08-queues-threads-tee/) | know which thread runs what, use `queue`/`queue2`/`multiqueue`, leaky queues, add/remove tee branches live | C |
| 09 | [C++ RAII wrapper](09-cpp-raii-wrapper/) | wrap GStreamer in `unique_ptr` deleters and a move-only `Pipeline` class; see why refcount mistakes leak | C++, C |
| 10 | [Custom element / plugin](10-custom-element-plugin/) | write a `GstBaseTransform` filter and a `GstPushSrc` source, register them in-process, build a real `.so` with meson | C |
| 11 | [Network streaming](11-network-streaming/) | stream RTP over UDP/TCP, set receiver caps, tune latency, understand an RTSP server | C, shell |
| 12 | [Debugging and tracing](12-debugging-and-tracing/) | use `GST_DEBUG` levels and categories, dump pipeline graphs, use the latency/leaks/stats tracers | C, shell |
| 13 | [Performance and zero copy](13-performance-zero-copy/) | reason about `GstBuffer`/`GstMemory`/pools, avoid copies and `videoconvert`, measure throughput | C |
| 14 | [Embedded TDA4VM](14-embedded-tda4vm/) | map pipelines onto the TDA4VM hardware (V4L2 codecs, TI `tiovx*`/`tidl*` elements), capture from cameras, cross-compile for aarch64 | C, shell |

**Suggested order:** 00 → 01 → 02 → 03 are the foundation, do them in order.
Then 04, 05, 06 (the everyday tools), 07 and 08 (control and threads), 09 if
you write C++. 10–14 are independent "advanced" tracks; do 12 early if you
get stuck debugging. Spend 1–2 sessions per lesson: read, run, break things
on purpose (exercises), then answer the self-test questions out loud.

## Setup

```bash
# development files (headers + pkg-config) and the runtime plugins
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
     gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good
# optional: needed only by some README/.sh examples (each says which)
sudo apt install gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
     gstreamer1.0-libav gstreamer1.0-rtsp graphviz
```

The **programs** need only core, plugins-base and plugins-good. Elements from
plugins-bad / ugly / libav / gst-rtsp-server / TI's SDK appear only in
README text and `.sh` scripts, and each one is marked with the package that
provides it. Check any element with `gst-inspect-1.0 <name>`.

## Build and run

From the repository root:

```bash
make run P=gstreamer/02-first-pipeline/01_hello_pipeline.c   # one program
./build/gstreamer/02-first-pipeline/01_hello_pipeline --display  # see the video
make gst            # build every GStreamer program
make test-gst       # build + run them all (headless, self-checking)
bash gstreamer/01-gst-launch/02_caps_filter.sh                # a shell pipeline
```

Programs build with `-Wall -Wextra -Wpedantic -Wshadow` and run under
AddressSanitizer + UBSan. They run with `ASAN_OPTIONS=detect_leaks=0`
because GLib/GStreamer keep some memory alive for the whole process on
purpose (type system, registry). To hunt *GStreamer object* leaks use the
leaks tracer instead: `GST_TRACERS=leaks GST_DEBUG=GST_TRACER:7 ./prog`
(lesson 12). Most programs accept `--display` to use `autovideosink`.

## Program conventions in this folder

* Header comment: concept, `make run` line, the README section to read, and
  an ASCII picture of the pipeline.
* A `CHECK(cond, msg)` macro asserts results (frame counts, caps fields,
  pixel values, refcounts, timing) and exits 1 on failure.
* Every reference is commented with its ownership: *transfer full* (you
  unref), *transfer none* (borrowed), *floating* (first bin sinks it).
* `*_bug.c` files are intentionally broken for the debugging lesson and are
  skipped by `make test-gst`.

## Cheat sheet of the most-used API

```c
gst_init(&argc, &argv);
GstElement *p = gst_parse_launch("videotestsrc ! fakesink", &err);  // or:
GstElement *e = gst_element_factory_make("videoconvert", "conv");    // floating
gst_bin_add_many(GST_BIN(p), a, b, NULL);  gst_element_link_many(a, b, NULL);
g_object_set(e, "prop", value, NULL);      g_signal_connect(e, "pad-added", cb, data);
gst_element_set_state(p, GST_STATE_PLAYING);
GstBus *bus = gst_element_get_bus(p);                                // unref
GstMessage *m = gst_bus_timed_pop_filtered(bus, timeout, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
gst_element_seek_simple(p, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 2 * GST_SECOND);
gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, probe_cb, data, NULL);
GstSample *s = gst_app_sink_pull_sample(GST_APP_SINK(sink));         // unref
gst_app_src_push_buffer(GST_APP_SRC(src), buf);                      // takes buf
gst_buffer_map(buf, &map, GST_MAP_READ); ... gst_buffer_unmap(buf, &map);
gst_element_set_state(p, GST_STATE_NULL); gst_object_unref(p);
```

## Remember forever (the course in 8 lines)

```
Pipeline of elements linked pad to pad; caps negotiated by intersection.
NULL→READY→PAUSED(preroll, ASYNC)→PLAYING; NULL before the last unref.
Bus: messages to the app. Events: with the data. Queries: synchronous Q&A.
Sometimes pads → pad-added; request pads → request + release.
Each source and each queue = a streaming thread; tee needs a queue per branch.
appsrc/appsink = the door between GStreamer and your code.
transfer full → unref; floating → bin sinks it; C++: static_cast flag enums.
Fast = no copies: dmabuf + pools + right formats; measure with tracers.
```

Review the cards on the schedule in [LEARNING_METHOD.md](../docs/LEARNING_METHOD.md).

## Further reading

* GStreamer Application Development Manual and Plugin Writer's Guide:
  <https://gstreamer.freedesktop.org/documentation/>
* `gst-inspect-1.0 <element>`: the most up-to-date reference for any element
  you have installed (including TI's on the board).
* TI Edge AI SDK documentation for the J721E / TDA4VM, for `tiovx*` and
  `tidl*` elements and the SDK image they require.
