# 08 — Queues, threads and tee: who runs what, and how to split a stream

## 1. Why this exists

Every GStreamer bug report about "my pipeline freezes", "latency grows to
seconds", "the display stutters when I start recording" or "it hangs at
PAUSED" comes down to one question: **which thread is pushing this buffer, and
where can it block?** Once you can draw the threads of a pipeline on paper you
can fix most of these in minutes.

This lesson answers:

- Where do streaming threads come from? (sources, queues, demuxers)
- Why does a `queue` change everything? (it is a thread boundary + a buffer)
- `queue` vs `queue2` vs `multiqueue` — when to use which.
- How to size and *leak* a queue for live video.
- Why `tee` needs a `queue` on every branch, and how to add/remove branches
  while the pipeline plays (start/stop recording).

## 2. Mental model

**A pipeline is a factory line. A source is a worker who carries each part
down the line by hand, through every machine, until they reach a buffer shelf
(a queue). There they put the part down and walk back for the next one. On the
other side of the shelf, a second worker picks parts up and carries them on.**

```
 worker T1 (videotestsrc0:src)          shelf        worker T2 (queue0:src)
 ┌────────────────────────────────┐   ┌───────┐   ┌──────────────────────────┐
 │ src ─► identity ─► videoconvert│──►│ queue │──►│ encoder ─► mux ─► sink   │
 └────────────────────────────────┘   └───────┘   └──────────────────────────┘
   T1 carries each buffer through        |  |        T2 carries it the rest
   these elements, as function calls     max-size    of the way
```

- An element *without* its own thread (identity, videoconvert, capsfilter,
  most filters) is just a function the worker calls: `chain(buffer)`.
- If any machine downstream is slow, the worker carrying the part waits — and
  so does everything upstream of it, until the nearest shelf.
- A shelf has a size. When it is full the upstream worker either **waits**
  (`leaky=no`, default) or **throws away a part** (`leaky=downstream`: the
  oldest one on the shelf; `leaky=upstream`: the new one).

A `tee` is a machine that hands *the same part* to several lines, one after
the other, **using the same worker**:

```
              ┌─[src_0]─► queue ║ T2 ─► display sink
 T1 ─► tee ───┤
              └─[src_1]─► queue ║ T3 ─► encoder ─► filesink
```

Without the queues, T1 would carry the frame all the way into the display sink,
come back, then carry it into the encoder. A slow encoder would stall the
display, and worse (see pitfalls) the pipeline can deadlock during preroll.

## 3. How it works

### Streaming threads

Threads are created by elements that own a `GstTask`:

| Element kind                         | Thread                                     |
|--------------------------------------|--------------------------------------------|
| Sources (`GstBaseSrc`, `v4l2src`, `videotestsrc`, `udpsrc`, `appsrc`) | one task on their src pad: loop "create buffer, push" |
| `queue`, `queue2`, `multiqueue`      | one task per src pad: loop "pop from shelf, push" |
| Demuxers in pull mode (e.g. `qtdemux` from a file) | one task on the sink pad pulling data |
| `appsrc`                             | internal queue + thread; your `push_buffer` just enqueues |
| Everything else                      | none: runs inside the caller's thread       |

Linux names the threads after the pad (`videotestsrc0:src`, `queue0:src`,
trimmed to 15 chars), so `top -H -p $(pidof myapp)` or `gdb`'s `info threads`
shows you exactly this picture. 01_thread_boundaries proves it with
`g_thread_self()` in pad probes:

```
run 1: videotestsrc ─► a ─► b ─► sink           a: videotestsrc0:s  b: videotestsrc0:s
run 2: videotestsrc ─► a ─► queue ─► b ─► sink  a: videotestsrc1:s  b: queue0:src
```

Your application thread (`main`) never carries buffers. It only changes
states, sends events and reads the bus. That is why bus messages exist: they
are the mailbox between the worker threads and you.

### queue properties: max-size and leaky

```
              ┌─────────── queue ───────────┐
  push ─────► │ [buf][buf][buf]  ...        │ ─────► pop (own thread)
              └─────────────────────────────┘
   full when ANY limit is reached:
     max-size-buffers (default 200)
     max-size-bytes   (default 10 MB)
     max-size-time    (default 1 s)       0 = no limit for that one
   leaky = no          full → pusher BLOCKS (no loss, latency grows)
           upstream    full → drop the NEW buffer
           downstream  full → drop the OLDEST buffer (keep freshest = live video)
   signals: overrun (became full), underrun (became empty), running, pushing
   min-threshold-*: do not push out until this much is queued (jitter buffer-ish)
```

03_leaky_queue measures it: a 100 fps live source, a sink that takes 20 ms per
frame, a 3-buffer queue.

```
  leaky=no         overruns=54 delivered=60/60  wall≈1200 ms  (source was slowed down)
  leaky=downstream overruns=27 delivered=33/60  wall≈ 670 ms  (half the frames dropped)
```

With `leaky=no` nothing is lost, but the source's thread had to wait. A
`videotestsrc` can wait; a real camera **cannot** — the V4L2 driver runs out of
buffers and drops frames itself, and your latency becomes "queue size × frame
time". For live preview you nearly always want `queue max-size-buffers=1..3
leaky=downstream` in front of the display, so it shows the *newest* frame.

### queue vs queue2 vs multiqueue

| Element      | What it is                                                                 | Use it for |
|--------------|-----------------------------------------------------------------------------|-----------|
| `queue`      | in-memory FIFO + thread                                                     | thread decoupling, tee branches, leaky live preview |
| `queue2`     | like queue, but can also buffer to disk (`temp-template`) and emits BUFFERING messages based on a byte/time watermark | network streams (HTTP), progressive download; `uridecodebin`/`playbin` insert it for you |
| `multiqueue` | N queues in one element, sized together; grows a queue if another stream is starving | after a demuxer (audio + video in one file) so neither stream starves the other; `decodebin` uses it |

`multiqueue` matters for A/V files: if the video queue is full and blocks the
demuxer while the audio queue is empty, the sink waits for audio forever →
deadlock. `multiqueue` notices and lets the full queue grow.

### tee: why every branch needs a queue

`tee` has an *always* sink pad and *request* src pads named `src_%u`. For each
buffer it does, in the upstream thread:

```c
for each src pad:  gst_pad_push(pad, gst_buffer_ref(buf));   /* same memory */
```

No copy — every branch sees the *same* `GstBuffer` (so never write to it
without `make_writable`, lesson 07). 02_tee_two_branches checks that both
appsinks receive all 50 frames with identical timestamps.

The deadlock without queues, step by step, during preroll (going to PAUSED):

```
 T1 ─► tee ─► sinkA      sinkA receives frame 1, prerolls: it BLOCKS T1
         └──► sinkB      inside its chain function until state = PLAYING.
                         sinkB never gets frame 1, never prerolls.
 The pipeline waits for ALL sinks to preroll before going to PLAYING
   → waits forever. Classic hang at PAUSED.
```

With a queue per branch, T1 only drops the frame onto each shelf and returns;
each sink blocks in its own thread. Rule: **after a tee, always a queue.**

`allow-not-linked=true` lets the tee keep running with zero linked branches
(otherwise it returns `not-linked` and the source stops) — needed when you add
branches on demand.

### Adding and removing branches at runtime (04_tee_add_remove_branch)

```
 ADD (app thread)                       REMOVE
 1. create queue ! sink, bin_add        1. IDLE probe on the tee's src_N pad
 2. link queue → sink                   2. in probe (streaming thread):
 3. sync_state_with_parent (→PLAYING)      unlink src_N ─╳─ queue.sink
 4. pad = request_pad_simple(tee,          send EOS into queue.sink (finalise file)
          "src_%u")                        return REMOVE
 5. gst_pad_link(pad, queue.sink)       3. app thread: set queue, sink → NULL
    (data starts flowing at once)          bin_remove, release_request_pad, unref
```

Order matters on ADD: bring the new elements to PLAYING *before* linking them,
or the first buffer hits an element in NULL and returns `FLUSHING`, which the
tee may turn into an error. On REMOVE, the state change to NULL happens in the
app thread (it has to join the branch's queue thread); doing it inside the
probe would make the streaming thread wait for itself.

For a real recording branch (encoder ! mp4mux ! filesink), wait for the EOS
to come out of the filesink before setting NULL — watch for it with a probe
on the filesink's sink pad, or set the pipeline's `message-forward=true` and
look for the forwarded EOS message on the bus — otherwise the MP4 trailer is
not written.

## 4. Complexity table

| Operation                     | Cost                                                 | Why |
|-------------------------------|------------------------------------------------------|-----|
| Element without thread        | one function call per buffer                         | runs in caller's thread; no context switch |
| queue push/pop                | O(1), mutex + cond signal, one context switch        | buffer pointer goes in a GQueue; no data copy |
| queue latency                 | up to `level × frame time`                           | a full 200-buffer queue at 30 fps = 6.6 s of delay |
| tee per branch                | O(1) ref per branch, **no copy**                     | same buffer, refcount +1 |
| writing to a tee'd buffer     | O(frame size) copy                                   | `make_writable` must copy (refcount > 1) |
| leaky=downstream drop         | O(1)                                                 | oldest buffer unreffed (returns to pool) |
| leaky=no when full            | upstream thread blocks                               | backpressure; live sources drop in the driver instead |
| thread count                  | 1 per source + 1 per queue (+ per multiqueue pad)    | each costs a stack (8 MB virtual) + scheduling |

## 5. C vs C++

All of this is plain C API; C++ changes nothing at the thread level. Two
things to watch in C++:

- callbacks that run in streaming threads (probes, appsink `new_sample`,
  `overrun` signal handlers) touch shared state from several threads → use
  `std::atomic<int>` (C: `g_atomic_int_*`, as in these programs) or a mutex.
- signal handlers are connected with `G_CALLBACK(fn)`; in C++ use a static
  function or a captureless lambda converted with `+[](GstElement*, gpointer){}`.

## 6. Where it is used in the real world

- **BeagleBone AI-64 camera app**: `v4l2src ! tee`, branch 1
  `queue leaky=downstream max-size-buffers=2 ! display`, branch 2
  `queue ! (TI hardware encoder) ! mp4mux ! filesink` added on demand, branch 3
  `queue leaky=downstream max-size-buffers=1 ! (scaler) ! inference`. The
  leaky queue before inference means the AI always works on the newest frame
  and never slows the camera down.
- **NVRs / dashcams**: tee for "record + live stream + snapshot".
- **playbin/decodebin**: `multiqueue` after the demuxer, `queue2` for network
  buffering (`BUFFERING` messages → pause until 100%).
- **WebRTC / RTSP servers**: one encoder, a tee, a branch per client added and
  removed dynamically with request pads (gst-rtsp-server shared media does this).

## 7. Common mistakes and pitfalls

- **No queue after tee** → hang at PAUSED (preroll deadlock) or one branch
  stalling the other.
- **Default queue in a live pipeline** → 1 s / 200 buffers of hidden latency.
  Set `max-size-*` explicitly; use `leaky=downstream` for preview.
- **Leaky queue *after* an encoder.** Dropping *raw* frames before an encoder
  is fine (the encoder just sees a lower frame rate). Dropping *encoded* H.264
  frames corrupts the picture until the next keyframe, because P-frames are
  differences against earlier frames.
- **Doing slow work in a callback** (probe, appsink callback) blocks that
  streaming thread and everything up to the previous queue.
- **Linking a new branch before its elements are PLAYING** → FLUSHING errors.
- **Setting the state of a branch to NULL from inside a probe** → deadlock.
- **Forgetting `gst_element_release_request_pad`** → the tee keeps a dead pad,
  and with `allow-not-linked=false` may error with not-linked.
- **Forgetting to unref the pad returned by `gst_element_request_pad_simple`**
  → leak (it returns a full ref *in addition* to the element's own).
- **Reading counters written in streaming threads without atomics** — data
  race; ThreadSanitizer will report it.

## 8. Files in this lesson

| File | What it shows | Run |
|------|---------------|-----|
| [01_thread_boundaries.c](01_thread_boundaries.c) | probes record `g_thread_self()`: same thread without queue, new thread after a queue | `make run P=gstreamer/08-queues-threads-tee/01_thread_boundaries.c` |
| [02_tee_two_branches.c](02_tee_two_branches.c) | tee → 2× (queue → appsink), both get every frame, same PTS | `make run P=gstreamer/08-queues-threads-tee/02_tee_two_branches.c` |
| [03_leaky_queue.c](03_leaky_queue.c) | slow sink: `leaky=no` blocks the producer, `leaky=downstream` drops (overrun signal) | `make run P=gstreamer/08-queues-threads-tee/03_leaky_queue.c` |
| [04_tee_add_remove_branch.c](04_tee_add_remove_branch.c) | request pad + sync_state to add a branch live, IDLE probe + release_request_pad to remove it | `make run P=gstreamer/08-queues-threads-tee/04_tee_add_remove_branch.c` |

Add `--display` to any built binary (e.g.
`./build/gstreamer/08-queues-threads-tee/02_tee_two_branches --display`) to
see the video in a window. Try it on the command line too:

```sh
gst-launch-1.0 videotestsrc num-buffers=100 ! tee name=t \
    t. ! queue ! fakesink  t. ! queue ! fakesink          # works
gst-launch-1.0 videotestsrc num-buffers=100 ! tee name=t \
    t. ! fakesink  t. ! fakesink                          # try: async sinks may hang
```

## 9. Exercises

1. *(easy)* In 01, add a second queue and assert there are now three distinct
   threads. Then run the program under `gdb` and type `info threads`.
2. *(easy)* In 03, try `leaky=upstream`. Which frames survive now, the oldest
   or the newest? Print the PTS of delivered frames to prove it.
3. *(medium)* In 03, replace the probe's sleep with `max-size-buffers=1` and an
   `identity sleep-time=20000`. Is the result the same? Why?
4. *(medium)* In 02, make branch B slow (probe that sleeps 30 ms) and measure
   how long branch A takes to finish, with and without B's queue. Explain.
5. *(hard)* In 04, make the branch `queue ! jpegenc ! avimux ! filesink` and
   finalise the file properly: wait until the EOS reaches the filesink before
   setting the branch to NULL. Check the file with `gst-discoverer-1.0`.
6. *(hard)* Build `videotestsrc ! tee` with 2 branches without queues using
   `gst-launch-1.0` and explain from the debug log
   (`GST_DEBUG=GST_STATES:5`) where it waits.

## 10. Remember forever

```
THREADS: sources + queues (+ demuxers in pull mode) own a thread; all other
         elements run inside the caller's thread. App thread never carries data.
QUEUE  = thread boundary + shelf. Full → leaky=no blocks, downstream drops oldest.
         Live preview: queue max-size-buffers=2 leaky=downstream.
queue (RAM) · queue2 (disk/network, BUFFERING msgs) · multiqueue (after demuxers)
TEE pushes the SAME buffer to each branch → queue on EVERY branch (preroll deadlock).
ADD branch:  elements → PLAYING, request_pad_simple("src_%u"), link.
DEL branch:  IDLE probe → unlink (+EOS) → app: NULL, bin_remove, release_request_pad, unref.
```

Review this card with the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

## 11. Self-test questions

1. Which elements create streaming threads? Draw the threads of
   `v4l2src ! videoconvert ! queue ! x264enc ! mp4mux ! filesink`.
2. What does a queue do when it is full with `leaky=no`? With
   `leaky=downstream`? Which one would you put in front of a display, and why?
3. What are the three `max-size-*` limits and what does `0` mean?
4. Explain the preroll deadlock of a tee without queues.
5. Does `tee` copy buffers? What happens when one branch writes to a buffer?
6. When would you choose `queue2` or `multiqueue` instead of `queue`?
7. Why must new branch elements reach PLAYING before you link them to the tee?
8. Why is the branch set to NULL from the app thread and not inside the IDLE
   probe?
