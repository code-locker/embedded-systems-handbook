# 06 — appsrc and appsink: your code in, your code out

> Prerequisites: [03-bus-and-messages](../03-bus-and-messages/README.md),
> [05-caps-and-negotiation](../05-caps-and-negotiation/README.md).

## 1. Why this exists

Pipelines built only from stock elements move media *between devices and
files*. Real applications need to touch the pixels themselves:

* **Frames out**: run your OpenCV / AI / measurement code on each camera
  frame → `appsink`.
* **Frames in**: you have frames from somewhere GStreamer doesn't know (a
  custom sensor SDK, a simulator, a rendered overlay, a proprietary network
  protocol) and want GStreamer to encode / stream / display them → `appsrc`.
* **Both**: GStreamer as a *processing engine* in the middle of your app
  (convert, scale, encode, decode) → `appsrc ! ... ! appsink`.

These two elements are the **bridge between GStreamer's threads and yours**.

## 2. Mental model

**Analogy — a factory line with a loading dock and a shipping dock.**

* `appsrc` is the **loading dock**: you drive up and unload crates
  (`push_buffer`). The dock has limited space (`max-bytes`); when it's full a
  worker either makes you wait (`block=true`) or rings "enough!"
  (`enough-data`); when it empties he rings "need more!" (`need-data`).
* `appsink` is the **shipping dock**: finished crates pile up on a shelf
  (`max-buffers`). You can come and collect (`pull_sample`, *pull mode*) or
  ask to be phoned for each crate (`new_sample` callback, *push mode*). If
  the shelf is full, the line stops (`drop=false`) or the oldest crate is
  thrown away (`drop=true`).

```
  YOUR THREAD             GSTREAMER (its own streaming threads)                YOUR THREAD
  ───────────     ┌─────────────────────────────────────────────────────┐     ───────────
  push_buffer ══► │ appsrc [queue]──►  convert ─► scale ─► encode ...    │
  end_of_stream   │   ▲ need-data / enough-data                         │
                  │                       ... ─► appsink [queue] ═══════╪═══► pull_sample
                  │                                 └─ new_sample() cb ─┼──►  (runs in the
                  └─────────────────────────────────────────────────────┘      streaming thread!)
```

**What you exchange:**

```
 appsrc input : GstBuffer   (bytes + PTS/DTS/duration + flags)   caps set once on appsrc
 appsink out  : GstSample = GstBuffer + GstCaps + GstSegment (+ info)
```

## 3. How it works

### Push mode: feeding your own frames (programs 03, 05)

1. Set **caps** on appsrc (what the bytes are) — without them nothing
   downstream can negotiate.
2. Set `format=GST_FORMAT_TIME` (timestamps in ns) — the default is BYTES,
   which is for byte streams, not frames.
3. For each frame: allocate a `GstBuffer`, fill it, set
   `GST_BUFFER_PTS` and `GST_BUFFER_DURATION`, `gst_app_src_push_buffer()`
   (**takes ownership**).
4. `gst_app_src_end_of_stream()` at the end → EOS flows after the queued data.

Live sources (cameras): set `is-live=true` and `do-timestamp=true` (appsrc
stamps buffers with the running time at arrival) — or stamp yourself from the
pipeline clock. Non-live sources (files, generated data): stamp
`n * frame_duration`.

`gst_app_src_push_buffer()` (function) **takes** the buffer; the
`"push-buffer"` action **signal** does **not** (you still unref). Mixing
them up is a classic leak or double free.

### need-data / enough-data: flow control (program 04)

```
  appsrc queue level
  max-bytes ┤········· enough-data ──► stop producing
            │      ╱╲       ╱╲
            │     ╱  ╲     ╱  ╲
            │ ___╱    ╲___╱    ╲___
            └─ need-data ──► start producing ───────────────► time
```

* `block=true`: `push_buffer` sleeps while the queue is full — simplest
  backpressure when you have a dedicated producer thread.
* signals/callbacks: turn your producer on/off (idle source, timer, socket
  read) — good with a GMainLoop, no extra thread.
* Callbacks may come from the streaming thread **or** from inside your own
  `push_buffer` call → protect shared state, never call blocking appsrc API
  while holding a lock the callback needs.

### Pull mode: the consumer asks (program 01)

```c
GstSample *s = gst_app_sink_try_pull_sample(sink, timeout);  /* NULL = EOS or timeout */
GstBuffer *b = gst_sample_get_buffer(s);                      /* borrowed */
GstCaps   *c = gst_sample_get_caps(s);                        /* borrowed */
gst_buffer_map(b, &map, GST_MAP_READ); ... gst_buffer_unmap(b, &map);
gst_sample_unref(s);                                          /* give it back */
```

`gst_app_sink_is_eos()` tells "ended" from "timed out". Prefer
`try_pull_sample` with a timeout over `pull_sample` (blocks forever).

### Signals vs callbacks, and which thread runs them (program 02)

| Way | Setup | Cost | Thread |
|-----|-------|------|--------|
| pull | nothing | none | your thread, blocking |
| `GstAppSinkCallbacks.new_sample` | `gst_app_sink_set_callbacks` | direct function call | **streaming thread** |
| `"new-sample"` signal | `emit-signals=true` + `g_signal_connect` | GObject marshalling per frame | **streaming thread** |

While your callback runs, everything upstream of the appsink in that thread
is **stalled**. Heavy work (AI inference) → copy/ref the sample and hand it to
a worker thread via a queue, or put a `queue` before appsink so it has its
own thread.

### appsink properties that matter

| Property | Meaning |
|----------|---------|
| `caps` | what you want (forces negotiation, e.g. `video/x-raw,format=RGB`) |
| `sync` | `true` = release frames at their clock time (real-time); `false` = as fast as possible |
| `max-buffers` | shelf size (0 = unlimited → unbounded memory if you're slow!) |
| `drop` | when full: `false` = block upstream, `true` = drop oldest (live preview / AI on latest frame) |
| `emit-signals` | only needed for the signal style |
| `wait-on-eos` | whether EOS waits until you pulled everything |

### appsrc properties that matter

| Property | Meaning |
|----------|---------|
| `caps` | format of what you push |
| `format` | `time` for frames |
| `is-live` | behaves like a camera (no preroll, latency matters) |
| `do-timestamp` | appsrc stamps buffers with clock running time |
| `max-bytes` / `max-buffers` / `max-time` | queue limits |
| `block` | `push_buffer` blocks when full |
| `stream-type` | `stream` (default), `seekable`, `random-access` (then `seek-data` callback) |
| `leaky-type` | (≥ 1.20) drop upstream/downstream instead of growing |

## 4. Complexity table

| Operation | Cost | Why |
|-----------|------|-----|
| `push_buffer` | O(1) + queue | buffer ref moves into appsrc's queue; no data copy |
| Filling a buffer from your array (`gst_buffer_fill`) | O(frame size) copy | the copy you pay if your data lives elsewhere; wrap instead to avoid it (`gst_buffer_new_wrapped_full`, lesson 13) |
| `pull_sample` | O(1) | hands you a ref to the same buffer the pipeline produced |
| `gst_buffer_map` READ, system memory | O(1) | just a pointer |
| `gst_buffer_map` on dmabuf/GPU memory | may be O(frame) + cache sync | mapping device memory can imply a copy or cache maintenance |
| `new_sample` callback | per frame, in streaming thread | your code's time adds directly to pipeline latency |
| Signal style | + ~µs per frame | GValue marshalling |
| Threads | appsrc adds 1 streaming thread | appsink adds none (it runs in its upstream's thread) |

## 5. C vs C++

Program 05 is the C++17 version of the full in→process→out loop:

* every GStreamer reference is a `std::unique_ptr<T, Deleter>`
  (`gst_object_unref`, `gst_sample_unref`, `gst_message_unref`,
  `gst_caps_unref`), so the `SamplePtr` in the loop body releases each frame
  even on `break`;
* a small `Pipeline` class sets NULL **before** the final unref in its
  destructor;
* producer and consumer are `std::thread`s with lambdas capturing the
  wrappers by reference; counters are `std::atomic<int>`;
* **flag enums need `static_cast`**: GStreamer's C headers declare
  `GstMessageType`, `GstMapFlags`, `GstSeekFlags`, `GstPadProbeType` as
  enums used as bit flags. In C, `GST_MESSAGE_EOS | GST_MESSAGE_ERROR` is an
  `int` that silently converts back; C++ has no implicit `int → enum`
  conversion, so write
  `static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR)`.
* callbacks must be plain functions or captureless lambdas (`+[](...){}`),
  with `this`/context passed as `user_data`.

## 6. Where it is used in the real world

* **Edge AI on the TDA4VM**: the classic BeagleBone AI-64 flow is camera →
  hardware scaler/color convert (TI elements) → `appsink` → your TIDL model
  call → `appsrc` with the overlay → encoder/display. TI's Edge AI apps
  (Python and C++) use exactly this appsrc/appsink structure when not
  using the `tidlinferer` element.
* **OpenCV integration**: `cv::VideoCapture` with the GStreamer backend is an
  appsink underneath; `cv::VideoWriter` is an appsrc.
* **Robotics**: ROS camera drivers push frames into GStreamer to encode /
  stream with `appsrc`.
* **Simulators / game engines**: rendered frames → `appsrc` → H.264 → RTP.
* **Test harnesses**: deterministic frames in, check frames out — like every
  program in this lesson.

## 7. Common mistakes and pitfalls

1. **No caps on appsrc** → `not-negotiated` or garbage interpretation.
2. **`format` left at BYTES** for video → timestamps/segments wrong.
3. **No timestamps** → sinks with `sync=true` drop or display at once;
   muxers fail. Set PTS + duration (or `do-timestamp=true` for live).
4. **Forgetting `end_of_stream`** → pipeline never reaches EOS; app hangs.
5. **Leak/double free** confusing `gst_app_src_push_buffer` (takes ownership)
   with the `push-buffer` signal (doesn't).
6. **Not unreffing samples** → the buffer pool runs dry and the pipeline
   **stalls** (looks like a deadlock, it's a leak).
7. **Heavy work in `new_sample`** → upstream stalls; with a live camera you
   get dropped frames and "pipeline is too slow" warnings.
8. **Unlimited queues** (`max-buffers=0`, no `max-bytes`) with a slow
   consumer → memory grows until OOM on a 4 GB board.
9. **`pull_sample` without timeout** → hangs forever if upstream errors;
   check the bus too.
10. **Ignoring stride** when reading pixels (lesson 05, program 04).
11. **Freeing your callback context** while the pipeline still runs: set the
    pipeline to NULL before your struct goes out of scope.

## 8. Files in this lesson

| File | What it shows | Run |
|------|---------------|-----|
| [01_appsink_pull.c](01_appsink_pull.c) | pull mode: `try_pull_sample` loop, map buffer, verify solid colour pixels and PTS steps | `make run P=gstreamer/06-appsrc-appsink/01_appsink_pull.c` |
| [02_appsink_callbacks.c](02_appsink_callbacks.c) | push mode: `GstAppSinkCallbacks` (`new_sample`, `eos`), atomics, proves callbacks run in the streaming thread | `make run P=gstreamer/06-appsrc-appsink/02_appsink_callbacks.c` |
| [03_appsrc_push.c](03_appsrc_push.c) | generate GRAY8 frames in C, PTS/duration, `push_buffer`, EOS; appsink checks bytes + timestamps | `make run P=gstreamer/06-appsrc-appsink/03_appsrc_push.c` |
| [04_appsrc_need_data.c](04_appsrc_need_data.c) | flow control: `need-data`/`enough-data` switch a GMainLoop idle producer on/off | `make run P=gstreamer/06-appsrc-appsink/04_appsrc_need_data.c` |
| [05_appsrc_to_appsink.cpp](05_appsrc_to_appsink.cpp) | C++17: producer thread → appsrc → convert+scale → appsink → consumer thread; unique_ptr deleters, `static_cast` flags | `make run P=gstreamer/06-appsrc-appsink/05_appsrc_to_appsink.cpp` |

All accept `--display` after building, e.g.
`./build/gstreamer/06-appsrc-appsink/03_appsrc_push --display`.
Build needs `sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev`.

## 9. Exercises

1. *(easy)* In 01 set `max-buffers=1 drop=true sync=true` and sleep 100 ms
   per pulled frame. How many frames do you get? Why is this the right
   setting for "AI on the latest frame"?
2. *(easy)* In 03 remove the `GST_BUFFER_PTS` lines and run with `--display`.
   What happens?
3. *(medium)* In 03 switch to `g_signal_emit_by_name(in, "push-buffer", buf, &ret)`.
   Run under the leaks tracer (`GST_TRACERS=leaks GST_DEBUG=GST_TRACER:7`)
   and fix the leak you introduced.
4. *(medium)* Make 02 hand each sample to a worker `GThread` through a
   `GAsyncQueue` (ref the sample, unref in the worker).
5. *(hard)* Add `is-live=true do-timestamp=true` to appsrc in 04 and push from
   a 30 Hz `g_timeout_add` instead of need-data. Measure end-to-end latency by
   comparing the running time at push vs at appsink.
6. *(hard)* Build an "encoder service": appsrc (raw I420) → `jpegenc` →
   appsink, returning JPEG bytes to the caller for each frame pushed.
7. *(board)* On the BeagleBone AI-64: `v4l2src ! videoconvert ! video/x-raw,format=RGB,width=224,height=224 ! appsink`
   and compute the mean pixel per frame; then replace `videoconvert`/scale with
   TI's hardware elements (TI SDK image) and compare CPU usage with `top`.

## 10. Remember forever

```
appsrc  = YOUR frames IN : caps + format=time + PTS/duration + push_buffer(takes ref) + end_of_stream
appsink = frames OUT     : try_pull_sample (pull) | callbacks.new_sample (push, STREAMING thread)
GstSample = buffer + caps + segment ; map -> read -> unmap -> gst_sample_unref
flow control: block=true | need-data / enough-data | appsink max-buffers + drop
unref every sample or the pool runs dry and the pipeline stalls
push_buffer() takes ownership; "push-buffer" SIGNAL does not
C++: unique_ptr<T,Deleter> per unref fn; static_cast<GstMessageType>(A|B)
```

Review this card with the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

## 11. Self-test questions

1. What three things must you set on appsrc before pushing video frames?
2. Who owns the buffer after `gst_app_src_push_buffer()`? After
   `g_signal_emit_by_name(src, "push-buffer", buf, &ret)`?
3. In which thread does `new_sample` run, and what is the consequence for
   upstream while it runs?
4. Difference between `max-buffers=1 drop=true` and `max-buffers=1 drop=false`?
5. When is `need-data` emitted, and from which threads can it arrive?
6. How do you tell, after `try_pull_sample` returned NULL, whether the
   stream ended or you timed out?
7. Why does forgetting `gst_sample_unref` make the pipeline *stop* rather
   than just use more memory?
8. Why does C++ need `static_cast` for `GST_MESSAGE_EOS | GST_MESSAGE_ERROR`?
