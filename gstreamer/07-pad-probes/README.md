# 07 — Pad probes: look at, drop, change and pause data at any pad

## 1. Why this exists

A pipeline built from stock elements is a sealed machine. Sooner or later you
need to reach *into* it:

- "How many frames really reach the encoder, and what are their timestamps?"
- "Drop every other frame" / "drop frames until the first keyframe".
- "Burn a marker into the image" without writing a whole element.
- "Swap the encoder / add a recording branch **while the camera keeps running**."

Pad probes do all of this. A probe is a callback attached to one pad; it is
called for every buffer, event or query crossing that pad, and it can let the
item pass, drop it, replace it, or **block the pad** so you can safely rewire
the pipeline.

## 2. Mental model

**A pad probe is a checkpoint on a conveyor belt.** The belt (the link between
two pads) carries boxes (buffers) and paperwork (events, queries). You put a
guard at the checkpoint. The guard can:

```
                     guard (your callback)
                           │
   ┌─────────┐  boxes  ┌───▼───┐        ┌─────────┐
   │ element ├─[src]──►│  ✋   ├──────►│[sink]   │ element
   │    A    │         └───────┘        │    B    │
   └─────────┘                          └─────────┘

   OK      wave it through              (inspect only)
   DROP    throw the box in the bin     (GStreamer unrefs it)
   modify  open the box, change it      (make_writable first!)
   BLOCK   stop the belt, hold the box  (rewire A→B safely, then release)
   REMOVE  wave it through and go home  (probe is removed)
```

Key fact you must never forget: **the guard works in the conveyor's thread,
not yours.** The callback runs in the *streaming thread* that is pushing the
data, at the moment it is pushing it. Whatever you do in there delays the
whole belt upstream of that pad.

```
  app thread (main)                 streaming thread "videotestsrc0:src"
  ─────────────────                 ──────────────────────────────────────
  gst_pad_add_probe(pad, cb) ──┐    create buffer
  ... waits on bus ...         │    push to videoconvert
                               └──► cb(pad, info)  ← runs HERE, every buffer
                                    push to sink
```

## 3. How it works

### Probe types (the mask you pass to `gst_pad_add_probe`)

| Mask bit                               | Called for                                       |
|----------------------------------------|--------------------------------------------------|
| `GST_PAD_PROBE_TYPE_BUFFER`            | every `GstBuffer` pushed                          |
| `GST_PAD_PROBE_TYPE_BUFFER_LIST`       | buffer lists (e.g. RTP payloaders push lists)     |
| `GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM`  | CAPS, SEGMENT, EOS, STREAM_START, TAG, ...        |
| `GST_PAD_PROBE_TYPE_EVENT_UPSTREAM`    | SEEK, QOS, RECONFIGURE, NAVIGATION, ...           |
| `GST_PAD_PROBE_TYPE_EVENT_FLUSH`       | FLUSH_START / FLUSH_STOP (must be asked for explicitly) |
| `GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM` / `_UPSTREAM` | CAPS, ACCEPT_CAPS, ALLOCATION, LATENCY, POSITION... |
| `GST_PAD_PROBE_TYPE_BLOCK`             | modifier: **hold** the data until the callback returns something other than OK, or the probe is removed |
| `GST_PAD_PROBE_TYPE_IDLE`              | called when the pad is *not* busy pushing (immediately if idle now); the pad stays blocked while you are in the callback |
| `GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM`  | shortcut = BLOCK + all downstream data            |

You OR them together: `BUFFER | EVENT_DOWNSTREAM` sees boxes and paperwork in
one callback; check `info->type` to know which one you got.

### Return values

| Return                     | Meaning                                                                    |
|----------------------------|-----------------------------------------------------------------------------|
| `GST_PAD_PROBE_OK`         | Pass the data. For a BLOCK probe: **stay blocked** (the thread waits).     |
| `GST_PAD_PROBE_DROP`       | Drop it (GStreamer unrefs it). Push returns OK upstream; queries fail.     |
| `GST_PAD_PROBE_REMOVE`     | Pass the data and remove this probe.                                       |
| `GST_PAD_PROBE_PASS`       | For BLOCK probes: let *this* item through but stay blocked for the next.   |
| `GST_PAD_PROBE_HANDLED`    | You consumed it yourself (you must unref it, and for queries fill the answer). |

Note the trap in the first row: on a BLOCK probe, `OK` means "keep blocking".
The usual pattern is to call `gst_pad_remove_probe()` inside the callback and
then return `OK`, which unblocks the pad when you return.

### Events you will see at a sink pad (01_probe_inspect)

```
 time ─────────────────────────────────────────────────────────────►
 STREAM_START  CAPS  SEGMENT  buf buf buf ... buf  EOS
 └──────── sticky events ──────┘
```

Sticky events are stored on the pad and re-sent automatically to any new peer
(see the swap recipe below). They always precede the first buffer, in this
order. A probe that drops CAPS or SEGMENT breaks negotiation: drop buffers, not
events, unless you know exactly why.

### Modifying buffers (03_probe_modify)

The buffer in a probe is shared: the source might keep it in a pool, a `tee`
may have handed the *same* buffer to another branch. Writing into a shared
buffer corrupts the other user's data. Rule:

```c
GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
buf = gst_buffer_make_writable(buf);   /* consumes our ref, maybe copies   */
GST_PAD_PROBE_INFO_DATA(info) = buf;   /* MUST put the (new) buffer back  */
gst_video_frame_map(&frame, &vinfo, buf, GST_MAP_WRITE);  /* strides!     */
```

`make_writable` is free when refcount == 1 and a full copy (memcpy of the frame)
otherwise. Use `gst_video_frame_map` rather than `gst_buffer_map` for video:
it gives you per-plane pointers and strides, which differ from `width` when the
hardware pads lines (very common on the TDA4VM, where lines are aligned for
DMA).

### Blocking probes: changing a running pipeline (04_probe_block_swap)

You may not unlink pads while data is flowing through them: the streaming
thread could be in the middle of a push. A blocking probe pauses the belt at a
known spot so the rewiring is safe. The canonical recipe (also in the
GStreamer docs, "Dynamic pipelines"):

```
 BEFORE:  queue ─[src]──► OLD ──► tee ─► ...

 1. add BLOCK_DOWNSTREAM probe on queue.src
         queue ─[src]✋    OLD ──► tee      (queue's thread waits in block_cb)

 2. in block_cb: remove the block probe, add an EOS probe on OLD.src,
    send EOS into OLD.sink  → OLD flushes out anything it still holds
         queue ─[src]✋ ──EOS──► OLD ──EOS──► ✋ (caught by EOS probe)

 3. in eos_cb (OLD fully drained):
       return DROP for that EOS (else the sink thinks the stream ended)
       set OLD to NULL, gst_bin_remove(OLD)
       gst_bin_add(NEW), link queue → NEW → tee, sync_state_with_parent(NEW)

 4. block_cb returns OK, the held buffer is pushed to the NEW peer
         queue ─[src]──► NEW ──► tee ─► ...
    NEW first receives the sticky STREAM_START/CAPS/SEGMENT automatically.
```

Why EOS? Elements such as encoders or muxers hold data internally (an encoder
may keep several frames for B-frame reordering). EOS makes them emit everything
before you remove them; otherwise those frames are lost or the muxer writes a
broken file. In 04 the old element is `identity`, which has no thread, so the
EOS travels synchronously and `eos_cb` runs *inside* `gst_pad_send_event()`. If
the old element contains a queue or its own thread, `eos_cb` runs later in that
other thread, and you must not return from `block_cb` until it has finished
(or use an IDLE probe and a different handshake).

`IDLE` probes are the lighter alternative when the element holds no data
(e.g. removing a `tee` branch, lesson 08): the callback runs when the pad is
between pushes, and while it runs, nothing is pushed.

## 4. Complexity table

| Operation                         | Cost                                                   | Why |
|-----------------------------------|--------------------------------------------------------|-----|
| Inspect probe (OK)                | O(1) per buffer, no copy                               | callback + pointer read in the streaming thread |
| DROP                              | O(1), buffer freed / back to pool                      | unref only |
| Modify, refcount 1                | O(pixels touched), no copy                             | `make_writable` returns the same buffer |
| Modify, shared buffer             | O(frame size) memcpy + O(pixels touched)               | `make_writable` copies the whole buffer |
| Slow callback (sleep, printf, I/O)| adds its time to **every** buffer's latency upstream   | runs in the streaming thread; the source cannot push the next frame |
| BLOCK probe                       | stalls the thread until you unblock                    | a held buffer stays in flight (latency, live sources may drop) |
| Element swap (04)                 | one drain (EOS) + relink + caps renegotiation          | NEW must negotiate; can reallocate pools |

## 5. C vs C++

The API is C; in C++ the callback must be a plain function or a captureless
lambda (a lambda with captures does not convert to a C function pointer). Pass
your object through `user_data` and give a `GDestroyNotify` if the probe owns
it:

```cpp
gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
    [](GstPad*, GstPadProbeInfo* info, gpointer ud) -> GstPadProbeReturn {
        static_cast<Counter*>(ud)->n++;
        return GST_PAD_PROBE_OK;
    }, counter, nullptr);
```

Mask arguments combine flags, so in C++ write
`static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM)`
(the `|` of two enum values is an `int`, which C++ will not convert back
implicitly — see lesson 09).

## 6. Where it is used in the real world

- **Frame counters / FPS / latency meters** in production apps (a BUFFER probe
  on the sink pad is the cheapest "is video still flowing?" watchdog).
- **Recording on demand**: a camera pipeline on a BeagleBone AI-64 keeps the
  preview running; when an event happens, an IDLE/BLOCK probe adds an
  `encoder ! mp4mux ! filesink` branch to the `tee`; later EOS is sent into
  that branch alone to finalise the file.
- **Keyframe gating**: drop buffers with `GST_BUFFER_FLAG_DELTA_UNIT` until the
  first keyframe, so a newly connected RTP client does not see grey garbage.
- **Metadata injection**: attach a `GstMeta` (e.g. TI Edge AI attaches
  inference results to buffers) or read it in a probe for overlays.
- `playbin` and `decodebin` themselves use blocking probes internally to switch
  streams and reconfigure.

## 7. Common mistakes and pitfalls

- **Doing slow work in the callback** (file I/O, `g_usleep`, logging every
  frame to a slow console). You stall the streaming thread; live sources then
  drop frames. Hand heavy work to another thread or an `appsink`.
- **Writing into a buffer without `gst_buffer_make_writable`** — silent data
  corruption in other `tee` branches, or a critical warning.
- **Forgetting to store the result of `make_writable`** back into
  `GST_PAD_PROBE_INFO_DATA(info)`: the original (now unreffed) buffer flows on
  — use-after-free.
- **Unreffing the buffer/event you were given** when returning OK or DROP. You
  only borrow it; DROP already unrefs. Only HANDLED transfers it to you.
- **Returning OK from a BLOCK probe and expecting flow to continue.** It stays
  blocked. Remove the probe (or return REMOVE/PASS).
- **Dropping sticky events** (CAPS, SEGMENT) — downstream errors with
  "not-negotiated" or timestamp chaos.
- **Calling `gst_element_set_state(pipeline, ...)` from inside a probe.**
  State changes wait for streaming threads to stop; you *are* the streaming
  thread → deadlock. Post a message or use `g_idle_add`/`gst_element_call_async`.
- **Removing an element without draining it** (no EOS): the last frames of an
  encoder are lost, an MP4 file has no `moov` atom and will not play.
- **Probe on a pad that does not exist yet** (sometimes pads of demuxers):
  add it in the `pad-added` handler (lesson 04).

## 8. Files in this lesson

| File | What it shows | Run |
|------|---------------|-----|
| [01_probe_inspect.c](01_probe_inspect.c) | BUFFER + EVENT probe: count frames, PTS monotonic, sticky event order, print caps | `make run P=gstreamer/07-pad-probes/01_probe_inspect.c` |
| [02_probe_drop.c](02_probe_drop.c) | `GST_PAD_PROBE_DROP` every odd frame; a 2nd probe proves half arrive | `make run P=gstreamer/07-pad-probes/02_probe_drop.c` |
| [03_probe_modify.c](03_probe_modify.c) | `make_writable` + `gst_video_frame_map`, paint a square, verify in appsink | `make run P=gstreamer/07-pad-probes/03_probe_modify.c` |
| [04_probe_block_swap.c](04_probe_block_swap.c) | BLOCK + EOS drain: replace `identity` with `videoflip` while PLAYING, no frame lost | `make run P=gstreamer/07-pad-probes/04_probe_block_swap.c` |

Run `make run ...` first (GStreamer programs need
`ASAN_OPTIONS=detect_leaks=0`, which `make test-gst` sets for you). Add
`--display` to the built binary to watch the video, e.g.
`./build/gstreamer/07-pad-probes/04_probe_block_swap --display` (the picture
flips upside down mid-stream).

## 9. Exercises

1. *(easy)* In 01, also listen to `GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM` and
   print which queries pass the sink pad (you will see CAPS, ACCEPT_CAPS,
   ALLOCATION). Hint: `GST_QUERY_TYPE_NAME(GST_PAD_PROBE_INFO_QUERY(info))`.
2. *(easy)* In 02, drop based on time instead of index: keep a frame only if
   its PTS is at least 100 ms after the last kept one. Assert ~10 fps out.
3. *(medium)* Make 02's probe remove itself (`GST_PAD_PROBE_REMOVE`) after 10
   dropped frames; compute the expected count and assert it.
4. *(medium)* In 03, paint the square at a position that moves with the frame
   number, and assert the position in appsink.
5. *(hard)* In 04, replace `identity` with `queue ! videoflip` in a bin (a
   ghost-padded `GstBin`). Now `eos_cb` runs in the inner queue's thread, so
   `block_cb` must wait for it (a `GMutex`+`GCond`, or do the relink in
   `eos_cb` and let `block_cb` return `GST_PAD_PROBE_OK` only after a flag is
   set). Explain why.
6. *(hard)* Gate on keyframes: build `videotestsrc ! jpegenc ! ...` and drop
   all buffers whose `GST_BUFFER_FLAG_DELTA_UNIT` is set (JPEG has none, so
   then try `vp8enc keyframe-max-dist=10`).

## 10. Remember forever

```
PAD PROBE = callback on a pad, runs IN THE STREAMING THREAD.
  gst_pad_add_probe(pad, MASK, cb, data, destroy) -> id ; gst_pad_remove_probe(pad,id)
  MASK: BUFFER | EVENT_DOWNSTREAM | QUERY_* | BLOCK | IDLE
  return OK (pass / keep blocking) DROP (bin it) REMOVE (pass+unhook) PASS HANDLED
  modify: buf = make_writable(buf); INFO_DATA(info) = buf; video_frame_map
  never unref borrowed data; never set pipeline state inside a probe
  swap live: BLOCK upstream src -> EOS into old -> on EOS out: DROP it,
             old->NULL, remove, add+link new, sync_state -> unblock
```

Review this card with the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

## 11. Self-test questions

1. In which thread does a probe callback run, and what happens to the pipeline
   while your callback sleeps for 50 ms?
2. What is the difference between returning `OK` from a normal probe and from a
   `BLOCK` probe?
3. Why must you call `gst_buffer_make_writable` before writing, and why must you
   store its return value?
4. Which events are sticky, in what order do they arrive, and what happens to
   them when you link a new element to a pad?
5. Why do we push EOS into the old element before removing it? What breaks if
   the old element is an H.264 encoder and you skip this?
6. Why did the EOS probe return `DROP`?
7. When is an `IDLE` probe enough and when do you need BLOCK + EOS drain?
8. Why may you not call `gst_element_set_state(pipeline, GST_STATE_NULL)` from
   inside a probe callback?
