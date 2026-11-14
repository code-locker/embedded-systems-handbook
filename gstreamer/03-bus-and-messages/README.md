# 03 — The bus, messages, events, queries and seeking

> Three ways things talk in GStreamer: **messages** go to *you* (the bus),
> **events** travel *along* the pipeline with the data, **queries** are
> questions with an immediate answer. Learn which is which and half of the
> GStreamer API becomes obvious.

## 1. Why this exists

The pipeline runs in its own threads. Your application must still learn
when it has finished, when something broke, how far it has played, and it
must be able to steer it: pause, seek, change speed. The **bus** delivers
news from the pipeline to the application thread safely. **Events** and
**queries** carry control in the other direction and between elements.
Without the bus your app cannot even tell that a camera was unplugged.

## 2. Mental model

**Analogy: a train line.**

* **Messages** = the station's **notice board** (bus). Trains and staff pin
  notes ("arrived at terminus" = EOS, "derailment" = ERROR). The manager
  (your app) reads the board in the office (main thread), when it wants to.
* **Events** = **special wagons coupled into the train**. A "new timetable"
  wagon (SEGMENT) or the "last wagon" sign (EOS) travel *in order* with the
  cargo (buffers). A SEEK is a **message radioed backwards** to the depot
  (source): "restart from km 200", and the depot first sends a
  "clear the track" (FLUSH) wagon.
* **Queries** = **phoning** another station: "where is the train now?"
  (POSITION), "what can you accept?" (CAPS). You wait on the line for the
  answer.

```
                    events (SEGMENT, CAPS, EOS) + buffers ─────────────►
   ┌─────┐          ┌────────┐          ┌────────┐
   │ src ├─────────►┤ filter ├─────────►┤  sink  │
   └──┬──┘          └───┬────┘          └───┬────┘
      │◄──────── upstream events (SEEK, QOS, RECONFIGURE) ─────┤
      │◄────────────── queries (POSITION, LATENCY) ────────────┤ (sync Q&A)
      │ post            │ post              │ post
      ▼                 ▼                   ▼
   ┌────────────────────────────── BUS ─────────────────────────────┐
   │ STATE_CHANGED  STREAM_START  ASYNC_DONE  ERROR  WARNING  EOS   │
   └──────────────────────────────┬─────────────────────────────────┘
                                  ▼  pop / watch callback
                         application (main thread)
```

## 3. How it works

### 3.1 The bus

Every pipeline has one bus (`gst_element_get_bus`, transfer full). Elements
call `gst_element_post_message()` from *any* thread; the bus queues the
message. Child bins forward their messages upward, so you only watch the
pipeline's bus. Two ways to read it:

```
 A) blocking pop (simple programs, tests)          B) main loop (real apps)
 ──────────────────────────────────────            ─────────────────────────
 msg = gst_bus_timed_pop_filtered(bus,             gst_bus_add_watch(bus, cb, data);
         timeout, EOS | ERROR);                    g_main_loop_run(loop);
 ... handle, gst_message_unref(msg)                cb(bus, msg, data) runs in the
                                                   main thread; return TRUE to keep
                                                   the watch; quit loop on EOS
```

A third, special one: `gst_bus_set_sync_handler()` calls you **in the posting
(streaming) thread**, before queueing. Used to hand a window handle to a
video sink (`prepare-window-handle`) at exactly the right moment. Do almost
nothing in it.

### 3.2 Message catalogue (the ones you will use)

| Message | Posted when | What you do |
|---------|-------------|-------------|
| `ERROR` | fatal problem; streaming stops | `gst_message_parse_error`, log, set NULL, maybe rebuild/retry |
| `WARNING` | non-fatal problem | log |
| `EOS` | *all* sinks received EOS | stop, or seek back to loop |
| `STATE_CHANGED` | an element changed state | filter `msg->src == pipeline` |
| `ASYNC_DONE` | an async state change (preroll, flushing seek) finished | now safe to query duration/seek |
| `STREAM_START` | a new stream begins | reset per-stream UI |
| `BUFFERING` | `queue2`/network source fill level (percent) | PAUSE below 100 %, PLAY at 100 % |
| `CLOCK_LOST` | clock provider removed | PAUSED then PLAYING to pick a new clock |
| `LATENCY` | latency changed (live) | `gst_bin_recalculate_latency` |
| `QOS` | an element dropped late data | stats, alert |
| `ELEMENT` | element-specific (e.g. `level` RMS, `GstMultiFileSink`) | read the structure |
| `APPLICATION` | **you** posted it | hand work from streaming thread to main thread |
| `TAG` | metadata found | title, codec, bitrate |

Program 01 prints the full sequence of a 5-frame pipeline:

```
 STATE_CHANGED (sink, src, pipeline) NULL→READY
 STATE_CHANGED src READY→PAUSED, STREAM_STATUS (thread created)
 STREAM_START
 STATE_CHANGED sink READY→PAUSED (got preroll buffer) → pipeline PAUSED
 ASYNC_DONE                       ◄ preroll finished
 LATENCY, NEW_CLOCK
 STATE_CHANGED ... PAUSED→PLAYING
 EOS
```

### 3.3 Errors and warnings (program 03)

An error is a `GError` with a **domain** and **code**, plus a **debug
string** for developers (file, function, element path, reason):

| Domain | Typical codes | Example |
|--------|---------------|---------|
| `GST_CORE_ERROR` | `NEGOTIATION`, `STATE_CHANGE`, `MISSING_PLUGIN` | internal problems |
| `GST_LIBRARY_ERROR` | `INIT`, `SETTINGS` | a codec library failed |
| `GST_RESOURCE_ERROR` | `NOT_FOUND`, `OPEN_READ`, `BUSY`, `NO_SPACE_LEFT` | file/device/socket |
| `GST_STREAM_ERROR` | `FAILED`, `DECODE`, `FORMAT`, `WRONG_TYPE` | bad data, flow errors |

The famous `Internal data stream error` (STREAM/FAILED) is a *symptom*: a
source's streaming loop got a bad **flow return** from downstream; the real
reason is in the debug string: `streaming stopped, reason not-negotiated (-4)`
(caps, lesson 05) or `not-linked (-1)` (an unlinked pad, lesson 04). Errors
may happen **during a state change** (`set_state` returns `FAILURE`: missing
file, busy camera) or **later while streaming**. Handle both. Classify by
`domain` + `code`, never by `message` text (it is translated!).

### 3.4 Handing work to the main thread (program 02)

Callbacks like `handoff`, `pad-added`, pad probes and appsink callbacks run
in a **streaming thread**. Don't block there, and don't touch state owned by
the main thread. The clean trick: post an `APPLICATION` message with a
`GstStructure` payload; the bus watch receives it in the main thread.

```
 streaming thread: gst_element_post_message(e, gst_message_new_application(
                       GST_OBJECT(e), gst_structure_new("progress", ...)));
 main thread:      bus_cb → case GST_MESSAGE_APPLICATION → update UI/state
```

(`g_idle_add` / `g_main_context_invoke` are the GLib-level alternatives.)

### 3.5 Events and queries

Events (`GstEvent`) are mini-objects sent with `gst_pad_push_event` /
`gst_element_send_event`. **Serialized** events (CAPS, SEGMENT, EOS, TAG)
stay in order with buffers; **non-serialized** ones (FLUSH_START, SEEK, QOS)
jump the queue.

```
 Downstream, serialized   : STREAM_START → CAPS → SEGMENT → buffers… → EOS
 Downstream, out of band  : FLUSH_START (unblock & discard) ... FLUSH_STOP
 Upstream                 : SEEK, QOS, NAVIGATION, RECONFIGURE, LATENCY
```

Queries (`GstQuery`) are sent with `gst_element_query` / `gst_pad_query`;
the answer is written into the query object; helpers:
`gst_element_query_position`, `gst_element_query_duration`,
`gst_query_new_seeking` + `gst_query_parse_seeking`.

### 3.6 Seeking and playback rate (program 04)

```
 gst_element_seek(pipeline, rate, GST_FORMAT_TIME, flags,
                  GST_SEEK_TYPE_SET, start, GST_SEEK_TYPE_SET, stop)

 1. app ──SEEK──► pipeline sends it to the sinks, they forward UPSTREAM
 2. source (or demuxer) handles it:
      FLUSH_START ─► everything downstream drops data, unblocks
      stop streaming thread, reposition
      FLUSH_STOP  ─►
      SEGMENT{start=2s, stop=3s, rate=2.0} ─►  then buffers from 2 s
 3. sinks preroll again (ASYNC_DONE), then play
 4. with rate=2.0 the sink maps running time = (PTS − start) / rate:
    1 s of media is shown in 0.5 s
```

Flags you combine: `FLUSH` (respond now; almost always), `ACCURATE` (exact
position, slower: decode from the previous keyframe and drop), `KEY_UNIT`
(snap to a keyframe, fast), `SEGMENT` (post `SEGMENT_DONE` instead of EOS,
for gapless looping), `TRICKMODE` (decoders may skip frames for fast
forward). Negative rate = reverse playback (needs demuxer/decoder support;
`videotestsrc` and many file formats do not support it).

Rules: seek in PAUSED or PLAYING, **after** preroll (`get_state` or
`ASYNC_DONE`); not in NULL/READY. Live sources (cameras) are not seekable:
check with the SEEKING query first.

## 4. Complexity table

| Operation | Cost | Why |
|-----------|------|-----|
| post a message | 1 alloc + lock | queued, not processed by the poster |
| `gst_bus_timed_pop_filtered` | O(messages skipped) | non-matching messages are dropped (unref'd) |
| bus watch dispatch | 1 main loop iteration per message | runs on the main context |
| serialized event | travels like a buffer | waits behind queued buffers in every `queue` |
| FLUSH | O(pipeline) | unblocks every pad, empties every queue |
| position query | walks to the sink | cheap, but don't poll faster than ~10 Hz |
| flushing seek in a file | demuxer index lookup + re-preroll | ms; `ACCURATE` adds decode from previous keyframe |

## 5. C vs C++

The API is identical; in C++ remember the flag casts:

```cpp
auto flags = static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE);
auto types = static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
```

Bus callbacks are plain C function pointers: pass a `this` pointer as
`user_data` and forward to a member function, or keep a
`std::function` per message type inside your wrapper (lesson 09 does this).
`GstMessage*` is a natural fit for `std::unique_ptr<GstMessage, Unref>`.

## 6. Where it is used in the real world

* **Camera recorder on the BeagleBone AI-64**: bus watch catches
  `RESOURCE/READ` errors when the USB camera disconnects, the app sets NULL,
  waits, rebuilds the pipeline.
* **Video players** (Totem, kiosk players): position/duration queries drive
  the progress bar, `gst_element_seek` the slider, `BUFFERING` messages the
  spinner for network streams.
* **Looping signage**: `SEGMENT` seek flag + `SEGMENT_DONE` message =
  gapless loop without EOS.
* **Test automation**: CI scripts wait for EOS with a timeout and fail on
  ERROR, exactly like program 01.

## 7. Common mistakes and pitfalls

1. **Never reading the bus** → errors are silent; the app "hangs".
2. **Multiple watches**: only one `gst_bus_add_watch` per bus; and if you add
   a watch, don't *also* pop from another thread.
3. **Forgetting `gst_message_unref`** after pop (each one leaks).
4. **Heavy work in a sync handler** or streaming-thread callback → stalls data.
5. **Seeking before preroll** (right after `set_state(PLAYING)` which returned
   `ASYNC`) → seek fails or is ignored.
6. **Comparing error messages as strings** → breaks with translations; use
   domain/code.
7. **Thinking EOS on the bus means one sink finished**: it is posted when
   *all* sinks got EOS.
8. **Expecting the position query to work in NULL/READY** → returns FALSE.
9. **Seeking a live source** → not seekable; check the SEEKING query.

## 8. Files in this lesson

| File | What it shows | Run |
|------|---------------|-----|
| [01_bus_message_tour.c](01_bus_message_tour.c) | pops *every* message and asserts their order | `make run P=gstreamer/03-bus-and-messages/01_bus_message_tour.c` |
| [02_bus_watch_mainloop.c](02_bus_watch_mainloop.c) | `GMainLoop` + `gst_bus_add_watch`, timer polling position, APPLICATION messages from the streaming thread | `make run P=gstreamer/03-bus-and-messages/02_bus_watch_mainloop.c` |
| [03_error_handling.c](03_error_handling.c) | resource error at READY, `not-negotiated` while streaming, a non-fatal WARNING; classify by domain/code | `make run P=gstreamer/03-bus-and-messages/03_error_handling.c` |
| [04_seek_and_rate.c](04_seek_and_rate.c) | SEEKING query, flushing accurate seek with rate 2.0, FLUSH/SEGMENT events seen by a probe, timing proves double speed | `make run P=gstreamer/03-bus-and-messages/04_seek_and_rate.c` |

## 9. Exercises

1. *(easy)* In program 01 add `GST_MESSAGE_STREAM_STATUS` parsing
   (`gst_message_parse_stream_status`) and print the thread's owner element.
2. *(easy)* In program 03 case A, use `/dev/null/x` instead: which code do you
   get now?
3. *(medium)* Change program 04 to rate 0.5. Update the timing check.
4. *(medium)* Make program 02 loop forever: on EOS, seek back to 0 instead of
   quitting; stop after 3 loops. Then do it with `GST_SEEK_FLAG_SEGMENT` and
   `SEGMENT_DONE` (no EOS between loops).
5. *(hard)* Write a bus **sync handler** that counts messages per type in the
   posting thread and prints the thread id, and compare with the watch.
6. *(hard, board)* On the BeagleBone AI-64, run a `v4l2src` pipeline and unplug
   the USB camera. Which error domain/code arrives? Implement auto-restart.

## 10. Remember forever

```
Messages → bus → app (EOS, ERROR, STATE_CHANGED, ASYNC_DONE, APPLICATION).
Events travel with data: down = CAPS, SEGMENT, EOS, FLUSH; up = SEEK, QOS.
Queries = synchronous questions: POSITION, DURATION, SEEKING, CAPS, LATENCY.
Real apps: gst_bus_add_watch + GMainLoop; callback runs in main thread.
Streaming thread → main thread: post an APPLICATION message.
Errors: classify by domain+code; "Internal data stream error" → read debug.
Seek after preroll: FLUSH (+ACCURATE|KEY_UNIT), rate in the SEGMENT.
```

Review this card on the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

## 11. Self-test questions

1. What is the difference between a message, an event and a query?
2. In which thread does a bus watch callback run? A sync handler?
3. Which message tells you preroll is complete?
4. A pipeline prints "Internal data stream error". Where do you look for the real reason?
5. What does a flushing seek send downstream, in which order?
6. How does the sink play twice as fast after a rate=2.0 seek?
7. Why must you not seek right after `set_state` returned `ASYNC`?
8. How do you safely notify the main thread from a `handoff` callback?
9. When is EOS posted on the bus if the pipeline has two sinks?
