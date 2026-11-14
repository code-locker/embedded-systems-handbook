# 02 — Your first pipeline in C (and C++)

> Six steps, always the same: **init, build, play, wait, stop, free**. Every
> GStreamer program you will ever write, including the big ones on the
> BeagleBone AI-64, is this skeleton plus details.

## 1. Why this exists

`gst-launch-1.0` is for experiments. A product needs a program: one that
reads its configuration, reacts to errors, restarts a camera, changes
pipelines at runtime and gets frames into its own code. This lesson turns
the shell one-liner into C: first with the same text syntax
(`gst_parse_launch`), then by creating and linking every element yourself,
then looks closely at **states**, because "why is my pipeline stuck in
PAUSED?" is the first real bug everybody meets.

## 2. Mental model

**Analogy: starting a factory line.**

1. Hire the machines (`gst_element_factory_make`).
2. Put them in the hall (`gst_bin_add`): the hall now *owns* them.
3. Connect the conveyor belts (`gst_element_link`).
4. Flip the main switch to PLAYING; the line runs **by itself** in its own
   threads.
5. The manager (your `main()`) sits by the intercom (**bus**) waiting for
   "finished" (EOS) or "fire!" (ERROR).
6. Switch everything off (NULL), then close the hall (unref).

```
   main thread                         streaming thread (created by GStreamer)
   ───────────                         ───────────────────────────────────────
   gst_init
   build pipeline
   set_state(PLAYING) ───────────────► videotestsrc loop: create frame,
   bus pop (blocks) ◄── EOS message ──  push → convert → sink ... × 60, EOS
   set_state(NULL)    ── joins thread ─►  (thread stops)
   unref
```

## 3. How it works

### 3.1 The 6-step skeleton (program 01)

```c
gst_init(&argc, &argv);                                   // 1 init
GstElement *p = gst_parse_launch("videotestsrc num-buffers=60 ! "
                                 "videoconvert ! fakesink", &err); // 2 build
gst_element_set_state(p, GST_STATE_PLAYING);              // 3 play
GstBus *bus = gst_element_get_bus(p);                     // 4 wait
GstMessage *m = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
                                 GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
gst_message_unref(m); gst_object_unref(bus);
gst_element_set_state(p, GST_STATE_NULL);                 // 5 stop
gst_object_unref(p);                                      // 6 free
```

Check `err` even when `p` is not NULL: with a missing element the parser
can return a *partial* pipeline **and** set the error.

### 3.2 Building by hand (program 02)

```
 gst_pipeline_new("p")              ─► empty pipeline (floating)
 gst_element_factory_make(f, name)  ─► element (floating)       ×4
 gst_bin_add_many(bin, a, b, c, d, NULL)   pipeline sinks + owns them
 gst_element_link_filtered(a, b, caps)     a ─[capsfilter]─► b
 gst_element_link_many(b, c, d, NULL)      b ─► c ─► d
```

`gst_element_link` only checks that the **pad templates** can be compatible
and picks free pads. The actual format is negotiated when data starts flowing
(lesson 05). Linking must happen *after* both elements are in the same bin.
To read the format that was really chosen:
`gst_pad_get_current_caps(gst_element_get_static_pad(e, "sink"))` (unref
both!).

When to use which:

| `gst_parse_launch` | factory_make + link |
|--------------------|---------------------|
| quick, same as the shell | a pointer to every element, no `get_by_name` |
| one error for the whole string | one error per link, precise messages |
| great for configurable pipelines (string from a config file) | needed for dynamic pads, custom linking logic |

Mixing is common: parse a description with named elements, then
`gst_bin_get_by_name(GST_BIN(p), "sink")` (transfer full: unref it).

### 3.3 States and preroll (program 03)

```
          set_state(PAUSED) returns ASYNC
                 │
 NULL ─► READY ─►│PAUSED───────────────────────────────►PLAYING
   open devices  │ source thread starts
                 │ buffer #0 travels down ─► sink receives it
                 │ sink BLOCKS on it (prerolled), posts ASYNC_DONE
                 └ get_state() now returns SUCCESS
```

* `gst_element_set_state` returns quickly. The return value tells you how:
  `SUCCESS`, `ASYNC` (still in progress), `NO_PREROLL` (live source: OK!),
  `FAILURE` (an element refused: device missing, caps impossible...).
* `gst_element_get_state(e, &cur, &pending, timeout)` **waits** for async
  changes. Use it when you need "really PAUSED" (e.g. before a seek or a
  duration query).
* **Preroll** is why a paused video player shows a picture and why seeking
  in PAUSED updates the image: the sink holds one buffer, ready to render.
* **Live sources** (camera `v4l2src`, `udpsrc`, `is-live=true`) only produce
  in PLAYING, so PAUSED returns `NO_PREROLL` and the sinks do not wait for
  data. A "stuck in PAUSED" pipeline is usually a sink that never gets its
  preroll buffer: a missing `queue` after a `tee`, an unlinked demuxer pad,
  or a muxer waiting for its second input.
* State changes go **from sinks to sources** inside a bin (downstream
  elements are ready before upstream ones start pushing). Going down
  (PLAYING → NULL) stops the streaming threads and joins them: so NULL
  before unref.

### 3.4 Where does my code run?

* `main()` = application thread. The bus pop happens here.
* Everything between source and sink runs in the **streaming thread**.
* Signal callbacks (`handoff`, `pad-added`, `new-sample`) run in the
  streaming thread too (lesson 03 shows how to get messages back into the
  main thread with a `GMainLoop`).

## 4. Complexity table

| Step | Cost | Why |
|------|------|-----|
| `gst_init` | ms; first ever run 100s of ms | plugin scan, registry cache |
| `gst_parse_launch` | µs per element + parse | factory lookups, property parsing from strings |
| `gst_element_link` | O(template caps) | template intersection, no data |
| READY | device/socket open | e.g. `v4l2src` opens `/dev/video0` |
| PAUSED (preroll) | one buffer through the whole pipeline | includes first caps negotiation and allocation |
| PLAYING | ~0 | clock starts, base time distributed |
| NULL | join all streaming threads | waits for the current buffer to finish |
| `gst_bus_timed_pop_filtered` | blocks, 0 CPU while waiting | condition variable |

## 5. C vs C++

Program 04 is program 01 in C++17. The differences:

* **RAII**: `std::unique_ptr<GstElement, PipelineDeleter>` sets state NULL and
  unrefs in the deleter, so early `return`s cannot leak. Lesson 09 builds the
  full wrapper.
* **Flag enums need a cast.** In C an enum is an `int`, so
  `GST_MESSAGE_EOS | GST_MESSAGE_ERROR` is accepted wherever a
  `GstMessageType` is expected. In C++ `enum | enum` yields `int`, and `int`
  does not implicitly convert to an enum:

  ```cpp
  // error: invalid conversion from 'int' to 'GstMessageType'
  gst_bus_timed_pop_filtered(bus, t, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  // correct
  gst_bus_timed_pop_filtered(bus, t,
      static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  ```
  The same applies to `GstPadProbeType`, `GstSeekFlags`, `GstMapFlags`,
  `GstBufferFlags`...
* Use `nullptr`, and `std::string` + `.c_str()` for dynamic descriptions.

## 6. Where it is used in the real world

* A camera-recording daemon on the BeagleBone AI-64: parse a pipeline from a
  config file (`v4l2src ! ... ! v4l2h264enc ! ... ! filesink`), PLAYING,
  watch the bus, restart on ERROR.
* Every unit test inside GStreamer itself uses this skeleton (`gst-check`).
* TI's Edge AI apps (`edgeai-gst-apps`) generate a gst-launch-style string
  from a YAML config and hand it to `gst_parse_launch`.

## 7. Common mistakes and pitfalls

1. **Not reading the bus** → errors are silent and the app hangs.
2. **Checking only `p != NULL`** after `gst_parse_launch`: also check `err`.
3. **Linking before adding to the same bin** → link fails.
4. **`g_object_set` without the final `NULL`** → reads garbage, crashes.
5. **Ignoring the `ASYNC` return value** and querying duration/position
   immediately → the query fails because preroll has not finished.
6. **Unref without NULL** → "Trying to dispose element ... but it is in
   PLAYING instead of the NULL state" critical warning, possible crash.
7. **Waiting forever** (`GST_CLOCK_TIME_NONE`) in tests: use a timeout so a
   broken pipeline fails instead of hanging CI.
8. **Waiting for EOS from a live source**: `is-live`/camera never sends it on
   its own; send `gst_element_send_event(p, gst_event_new_eos())`.

## 8. Files in this lesson

| File | What it shows | Run |
|------|---------------|-----|
| [01_hello_pipeline.c](01_hello_pipeline.c) | the 6-step skeleton with `gst_parse_launch`; `--display` opens a window | `make run P=gstreamer/02-first-pipeline/01_hello_pipeline.c` |
| [02_build_and_link.c](02_build_and_link.c) | factory_make, bin_add_many, link_filtered, link_many, reading negotiated caps | `make run P=gstreamer/02-first-pipeline/02_build_and_link.c` |
| [03_states_and_preroll.c](03_states_and_preroll.c) | ASYNC preroll vs live NO_PREROLL, `get_state`, the sink's prerolled `last-sample` | `make run P=gstreamer/02-first-pipeline/03_states_and_preroll.c` |
| [04_first_pipeline.cpp](04_first_pipeline.cpp) | C++17 with `unique_ptr` deleters, `static_cast<GstMessageType>` | `make run P=gstreamer/02-first-pipeline/04_first_pipeline.cpp` |

Needs `sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev`.
To see the video: `./build/gstreamer/02-first-pipeline/01_hello_pipeline --display`.

## 9. Exercises

1. *(easy)* In program 01, replace `fakesink sync=false` by
   `fakesink sync=true`. How long does it take now, and why ~2 s? (Note:
   plain `fakesink` defaults to `sync=false`; display sinks default to true.)
2. *(easy)* In program 02, make `videoflip` rotate 90° (`method=clockwise`)
   and change the check: what are the negotiated width/height *after* the flip?
3. *(medium)* Add a bus timeout of 100 ms in program 01 and handle the
   timeout by printing the current state with `gst_element_get_state`.
4. *(medium)* In program 03, add a `tee` without queues before the sink and
   two fakesinks. Observe `get_state` time out in PAUSED (that is the
   classic tee deadlock, lesson 08).
5. *(hard)* Write a C program that takes the pipeline description from
   `argv[1]` (a mini gst-launch) and prints every STATE_CHANGED message of the
   top-level pipeline.

## 10. Remember forever

```
init → build (parse_launch | factory_make + bin_add + link) → PLAYING
     → bus pop EOS|ERROR → NULL → unref.
bin_add takes ownership; link only after both are in the same bin.
PAUSED = preroll: ASYNC until each sink holds 1 buffer.
Live source: PAUSED returns NO_PREROLL (normal!).
get_state(timeout) waits for async changes. NULL before the last unref.
C++: static_cast<GstMessageType>(A | B).
```

Review this card on the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

## 11. Self-test questions

1. Recite the 6 steps of a GStreamer program.
2. Who owns an element after `gst_bin_add`? What happens to your pointer?
3. What does `gst_element_link` check, and what does it *not* check?
4. Why does `set_state(PAUSED)` return `ASYNC`? What finishes it?
5. What does `NO_PREROLL` mean, and is it an error?
6. Name two causes of a pipeline stuck in PAUSED.
7. Why must you set NULL before the final unref?
8. Why does `GST_MESSAGE_EOS | GST_MESSAGE_ERROR` not compile as a `GstMessageType` in C++?
