# 12 — Debugging and tracing

> You cannot attach a debugger to "the video is black". You *can* make the
> pipeline tell you, element by element, what it negotiated, what it pushed,
> how long each step took, and which object you forgot to unref.

## 1. Why this exists

A GStreamer pipeline is a set of threads passing buffers through plugins you did
not write. When it fails, the symptom is usually far away from the cause:

| Symptom you see                         | Typical real cause                                    |
|-----------------------------------------|-------------------------------------------------------|
| `Internal data stream error`            | an element upstream returned `not-negotiated`         |
| pipeline hangs in PAUSED                | a sink never prerolled (tee without queue, no data)   |
| video plays at 2 fps on the board       | an unwanted `videoconvert` doing per-pixel CPU work   |
| memory grows slowly for hours           | a `GstSample` / `GstPad` / `GstCaps` ref never dropped|
| 400 ms latency on a "live" camera       | a queue filling up, or sink `sync` + big jitterbuffer |

GStreamer has four built-in tools that turn these into readable facts:
**the debug log** (`GST_DEBUG`), **pipeline graphs** (`GST_DEBUG_DUMP_DOT_DIR`),
**inspection tools** (`gst-inspect-1.0`, `gst-launch-1.0 -v -m`,
`gst-discoverer-1.0`), and **tracers** (`GST_TRACERS`: latency, leaks, stats,
rusage). This lesson teaches all four, plus how to put a debugger on a
streaming thread.

## 2. Mental model

**Analogy: a factory with CCTV, a floor plan, and stopwatches.**

- The **debug log** is the CCTV: every machine (element) has its own camera
  (a *debug category*) and you choose how closely each camera records
  (the *level*). Turning every camera to maximum fills the disk in seconds, so
  you zoom in on one machine.
- The **dot graph** is the floor plan, printed while the line is running, with
  the exact part numbers (caps) written on every conveyor (link).
- **Tracers** are stopwatches and inventory counters bolted onto the conveyors:
  they time each box (latency), count boxes (stats), measure electricity (CPU),
  and at closing time list every box nobody took home (leaks).

```
                 your app
                    │ gst_init() reads GST_DEBUG, GST_TRACERS, GST_DEBUG_DUMP_DOT_DIR
   ┌────────────────┼─────────────────────────────────────────────────────┐
   │ videotestsrc ──┼──► queue ─────► videoconvert ─────► fakesink         │
   │   cat:videotestsrc   cat:queue_dataflow  cat:videoconvert  cat:fakesink│
   │     ▲               ▲  hooks: pad-push-pre / pad-push-post ▲          │
   │     └────── tracers (latency, stats, leaks, rusage) ───────┘          │
   └───────────────────────────────┬──────────────────────────────────────┘
                                   │ every log line: (category, level, object, text)
                    ┌──────────────┴──────────────┐
                    ▼                             ▼
         gst_debug_log_default            your log function
         -> stderr / GST_DEBUG_FILE       (01, 03 capture lines here)
```

## 3. How it works

### 3.1 GST_DEBUG levels and categories

| Level | Name     | What you get                                        | Use when                     |
|------:|----------|-----------------------------------------------------|------------------------------|
| 0     | none     | nothing                                             |                              |
| 1     | ERROR    | fatal problems                                      |                              |
| 2     | WARNING  | non-fatal problems — **always run with this**       | default in production        |
| 3     | FIXME    | known incomplete code paths hit                     | "why is this format slow?"   |
| 4     | INFO     | state changes, important one-off events             | lifecycle questions          |
| 5     | DEBUG    | per-negotiation/per-event detail                    | caps problems                |
| 6     | LOG      | per-buffer detail (huge)                            | one element, short runs      |
| 7     | TRACE    | per-refcount, tracer records                        | tracers, leak hunts          |
| 9     | MEMDUMP  | hex dumps of buffers                                | protocol parsing             |

Syntax: `GST_DEBUG=<default>,<glob>:<level>,...` — e.g.

```
GST_DEBUG=2                         # warnings everywhere (start here, always)
GST_DEBUG=2,v4l2src:5               # plus one element in detail
GST_DEBUG=2,*CAPS*:5,GST_PADS:5     # the negotiation story
GST_DEBUG=2,*dataflow*:6            # who pushes what (queues)
GST_DEBUG=*:6                       # everything — only for a 1-second run
GST_DEBUG_FILE=/tmp/g.log           # write to a file, not stderr
GST_DEBUG_NO_COLOR=1                # no ANSI codes (for grep / files)
gst-launch-1.0 --gst-debug-help     # list every category that exists
```

A log line looks like this — learn to read the columns:

```
0:00:00.004153570 81892 0x5970ae863c40 DEBUG  GST_CAPS gstutils.c:3120:gst_pad_query_caps:<videotestsrc0:src> query returned ...
└─ running time ─┘└pid┘ └── thread ──┘ level  category  file:line:function     :<object>       message
```

The **thread column** is gold: two different values around a `queue` show the
thread boundary from lesson 08.

Your own code gets a category in three lines (program `01`):

```c
GST_DEBUG_CATEGORY_STATIC(my_cat);
#define GST_CAT_DEFAULT my_cat
GST_DEBUG_CATEGORY_INIT(my_cat, "mydemo", 0, "my app");   /* after gst_init */
GST_INFO_OBJECT(pipeline, "started");   /* -> GST_DEBUG=mydemo:4 shows it */
```

When the level is below threshold, the macro is one integer comparison — leave
the calls in production code. Build with `-DGST_DISABLE_GST_DEBUG` (or a
GStreamer built with `-Dgst_debug=false`, common on small Yocto images) and
they vanish entirely; that is why some board images print nothing at all.

> **C pitfall shown in `01`.** `gst_debug_add_log_function` /
> `gst_debug_remove_log_function` are also *macros* in `gstinfo.h` that compare
> a function pointer with `(void *)`; under `-Wpedantic` that warns. Writing
> `(gst_debug_add_log_function)(fn, data, NULL)` calls the real function and
> skips the macro. Pass `NULL` to mean "the default stderr printer".

### 3.2 Pipeline graphs (dot)

```
export GST_DEBUG_DUMP_DOT_DIR=/tmp/dot     # BEFORE gst_init / gst-launch
gst-launch-1.0 ...                         # dumps at every state change:
  0.00.00.006-gst-launch.READY_PAUSED.dot  #   caps being set up
  0.00.00.007-gst-launch.PAUSED_PLAYING.dot#   negotiated caps on every link
dot -Tpng file.dot -o graph.png            # sudo apt install graphviz (or xdot)
```

In your own program you choose the moment (`02`):

```c
GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "name");
GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "name");
```

Best moments: right after preroll (PAUSED), inside an ERROR handler, and inside
a `pad-added` callback after linking. What the graph shows:

```
 ┌videotestsrc0──────┐   video/x-raw            ┌capsfilter0┐       ┌tee┐── src_0 ─► queue0 ─► sinkA
 │             src ●─┼── format: I420 ─────────►● sink  src ●──...─►●   │
 └───────────────────┘   width: 320, ...        └───────────┘       └───┘── src_1 ─► queue1 ─► sinkB
   element boxes,  ●  pads (colour = direction),  edge label = negotiated caps
   a pad drawn with no edge = NOT LINKED (the classic decodebin bug)
```

### 3.3 Inspection tools

| Tool | What it answers |
|------|-----------------|
| `gst-inspect-1.0` | which plugins exist; `gst-inspect-1.0 x264enc` → pad templates (caps), properties with ranges/defaults, signals, rank |
| `gst-inspect-1.0 -b` | blacklisted plugins (failed to load — missing .so dependency!) |
| `gst-launch-1.0 -v` | prints the caps every pad negotiated (`notify::caps`) and property changes |
| `gst-launch-1.0 -m` | prints every bus message (state-changed, stream-status, qos, latency, eos) |
| `gst-launch-1.0 -e` | on Ctrl-C send EOS first (so muxers write a valid file) |
| `gst-discoverer-1.0 file.mp4` | container, streams, codecs, duration, tags of a file/URI |
| `gst-play-1.0` | quick playback with keyboard seeking |

A sharp trick from `01_debug_levels.sh`: `gst-launch-1.0 -v videotestsrc ! fakesink`
negotiates **A444_16LE** — the first format in videotestsrc's list — because
nothing downstream asked for anything. Unconstrained caps pick surprising
formats; always state the format you want.

### 3.4 Tracers

Tracers hook core events (pad push, query, state change, object create/free)
without changing any element. Enabled by `GST_TRACERS` (read in `gst_init`),
output on category `GST_TRACER` at level 7:

```
GST_TRACERS="latency(flags=pipeline+element)" GST_DEBUG=GST_TRACER:7 gst-launch-1.0 ...
GST_TRACERS="leaks(stack-traces-flags=full)"  GST_DEBUG=GST_TRACER:7 ./myapp
GST_TRACERS=stats GST_DEBUG=GST_TRACER:7 GST_DEBUG_FILE=t.log ...; gst-stats-1.0 t.log
GST_TRACERS="rusage;latency" ...           # several, separated by ';'
```

| Tracer (core) | Measures | Record name |
|---------------|----------|-------------|
| `latency` | time from a source pad push to the sink receiving the same buffer; `flags=element` adds per-element time, `flags=reported` the latency each element reports | `latency`, `element-latency`, `element-reported-latency` |
| `leaks` | GstObjects/GstMiniObjects alive at `gst_deinit()` (or on SIGUSR1/2 snapshots) | `object-alive` |
| `stats` | buffers/events/queries per pad and thread; summarised by `gst-stats-1.0` | many |
| `rusage` | CPU load per process and per thread | `proc-rusage`, `thread-rusage` |
| `log` | logs every hook (very verbose) | — |
| `factories` | which element factories got used (good for trimming an image) | — |

**Not in core:** `interlatency`, `proctime`, `framerate`, `cpuusage`,
`queuelevel`, `scheduletime`, `bitrate`, `buffer` come from RidgeRun's
**GstShark** (`github.com/RidgeRun/gst-shark`), a separate project you build
and install yourself (on the board too). `interlatency` is the one you want on
a TDA4VM pipeline: it shows the time from the source to *each* element, so you
see whether the encoder or the scaler eats your budget.

### 3.5 Finding reference leaks

ASan's leak checker is disabled for GStreamer programs in this repo
(`ASAN_OPTIONS=detect_leaks=0`) because GLib keeps type-system memory for the
life of the process. The tool for refcount leaks is the **leaks tracer**:

```
GST_TRACERS=leaks GST_DEBUG=GST_TRACER:7 ./build/gstreamer/12-debugging-and-tracing/04_refleak_bug
... object-alive, type-name=(string)GstPad, ... description=(string)<'':src>, ref-count=(uint)1
... object-alive, type-name=(string)GstCaps, ... description=(string)video/x-raw, width=(int)320
```

Requirement: call `gst_deinit()` at the end of `main()` — that is when the
tracer prints its list. Add `stack-traces-flags=full` to see where each
leaked object was created; `filters=GstPad,GstBuffer` to narrow it.

### 3.6 gdb on streaming threads

```
G_DEBUG=fatal-warnings gdb --args ./myapp      # stop on the first g_warning/g_critical
(gdb) run
(gdb) thread apply all bt                      # every streaming thread's stack
(gdb) info threads                             # names like "videotestsrc0:src", "queue0:src"
```

GStreamer names its streaming threads after the pad that owns the task, so in
`info threads` and `top -H` you see which element is spinning. `G_DEBUG=fatal-criticals`
is the softer version (stops on `g_return_if_fail` failures — usually a NULL
pointer passed to a GStreamer API).

### 3.7 Common error messages decoded

| Message | Meaning | First thing to try |
|---------|---------|--------------------|
| `no element "x264enc"` | plugin not installed / blacklisted | `gst-inspect-1.0 x264enc`; `gst-inspect-1.0 -b`; delete `~/.cache/gstreamer-1.0` |
| `could not link a to b` | no common caps between templates | `gst-inspect-1.0 a b`, compare pad templates; add `videoconvert` to find out |
| `Internal data stream error` + `reason not-negotiated (-4)` | caps fixed upstream don't fit downstream | `GST_DEBUG=2,*CAPS*:5`; dump dot at the error |
| `reason not-linked (-1)` | a pad pushed into nothing (decodebin pad never linked) | handle `pad-added`; dot graph shows the dangling pad |
| `reason error (-5)` | an element failed internally (device, file) | look at the ERROR message *before* this one |
| pipeline stuck in PAUSED, `ASYNC_START` with no `ASYNC_DONE` | a sink never got a buffer to preroll | tee/demux branch without queue; live source → use `is-live`, sinks `async=false` |
| `There may be a timestamping problem, or this computer is too slow` | buffers arrive late at a `sync=true` sink | QoS; check CPU with `rusage`; try `sync=false` to confirm |

## 4. Complexity table

| Tool | CPU cost | Memory | Latency impact | Why |
|------|----------|--------|----------------|-----|
| `GST_DEBUG` below threshold | ~1 compare per call site | 0 | none | macro checks the category level first |
| `GST_DEBUG=*:5` to stderr | very high | low | huge, changes timing | formats + writes every line synchronously, holding a lock |
| same to `GST_DEBUG_FILE` | high | low | large | file I/O is cheaper than a terminal, still serialising |
| dot dump | O(elements + pads) once | one text file | one-off stall | walks the bin tree under locks |
| `latency` tracer | O(1) per buffer per pad | small | small (µs) | stores a timestamp meta on each buffer |
| `stats` tracer | O(1) per pad push | O(pads) | small | counters per pad |
| `leaks` tracer | O(1) per object create/free | O(live objects) + stack traces | small, larger with stacks | hash table of live objects |
| `rusage` | one `getrusage` per push | tiny | small | |
| `gst-launch -v` | tiny | 0 | none | only prints on caps changes |

Heisenbugs: heavy logging changes thread timing and can hide a race. Use the
lowest level that answers the question, and prefer tracers for timing.

## 5. C vs C++

Everything here is environment variables and C macros; C++ uses the same
calls. Points worth knowing:

- `GST_DEBUG_CATEGORY_STATIC` / `GST_INFO_OBJECT` work unchanged in C++.
- The log hook is a plain C function pointer: in C++ use a captureless lambda
  (`+[](GstDebugCategory *, ...) {}`) or a `static` function; pass `this` via
  `user_data`.
- The `(gst_debug_add_log_function)(...)` trick is needed in C++ too (the same
  macro is expanded; C++ is even stricter about function→`void*` conversion).
- `GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(p.get()), ...)` with a `unique_ptr`
  pipeline from lesson 09.

## 6. Where it is used in the real world

- **Every GStreamer bug report** upstream is expected to include a
  `GST_DEBUG=*:6` log of a short run and a dot graph.
- **BeagleBone AI-64 / TDA4VM bring-up:** a camera that delivers nothing is
  diagnosed with `GST_DEBUG=2,v4l2*:5` (shows the `VIDIOC_*` ioctls and the
  formats the driver offered); a TI OVX element that refuses to link is
  diagnosed with `gst-inspect-1.0 tiovxmultiscaler` on the board plus a dot
  graph. GstShark `interlatency` is the standard way to find which hardware
  block (ISP, MSC, encoder) owns the latency.
- **Embedded products** ship with `GST_DEBUG=2` piped to the system journal,
  and a debug switch that dumps dot graphs on error.
- **CI**: the leaks tracer (`GST_TRACERS=leaks` and failing on `object-alive`)
  is how GStreamer's own test suite and many companies catch refcount leaks.
- **Image size work** (Yocto): the `factories` tracer tells you exactly which
  plugins your product pipelines load.

## 7. Common mistakes and pitfalls

1. **Setting `GST_DEBUG` / `GST_TRACERS` / `GST_DEBUG_DUMP_DOT_DIR` after
   `gst_init()`.** They are read once at init. Use `g_setenv` before
   `gst_init` (programs `02`, `03`), or the API (`gst_debug_set_threshold_for_name`).
2. **`GST_DEBUG=*:9` on a video pipeline.** Gigabytes in seconds, timing
   destroyed. Glob to the category you care about.
3. **Reading only the last error.** `Internal data stream error` is the
   *consequence*; the cause is the first WARNING/ERROR above it.
4. **No `gst_deinit()`** → the leaks tracer prints nothing at exit.
5. **Thinking `interlatency` is missing from your install.** It was never in
   core; it is GstShark.
6. **Debug log in a GStreamer built without debug** (`-Dgst_debug=false`):
   nothing prints, no error. Check with `gst_debug_is_active()` or
   `gst-launch-1.0 --gst-debug-help` (empty list).
7. **Log function that is not thread-safe.** It runs in every streaming
   thread. Atomics or a mutex, never plain `count++`.
8. **Stale registry cache** after installing/removing a plugin by hand:
   `rm -rf ~/.cache/gstreamer-1.0`.

## 8. Files in this lesson

| File | What it shows | Run |
|------|---------------|-----|
| [01_debug_categories.c](01_debug_categories.c) | own category, per-category thresholds from code, a log hook that counts lines, proof that LOG is filtered at DEBUG | `make run P=gstreamer/12-debugging-and-tracing/01_debug_categories.c` |
| [02_dot_dump.c](02_dot_dump.c) | `GST_DEBUG_DUMP_DOT_DIR` set before init, dumps at PAUSED/PLAYING, checks the .dot mentions the elements and negotiated width (`KEEP_DOT=1` keeps it) | `make run P=gstreamer/12-debugging-and-tracing/02_dot_dump.c` |
| [03_tracers_in_code.c](03_tracers_in_code.c) | `GST_TRACERS="latency(flags=pipeline+element);stats"` from code, captures and counts the records | `make run P=gstreamer/12-debugging-and-tracing/03_tracers_in_code.c` |
| [04_refleak_bug.c](04_refleak_bug.c) | **intentional** pad + caps leak; find them with `GST_TRACERS=leaks GST_DEBUG=GST_TRACER:7` (skipped by `make test-gst`) | `make run P=gstreamer/12-debugging-and-tracing/04_refleak_bug.c` |
| [01_debug_levels.sh](01_debug_levels.sh) | levels, globs, `GST_DEBUG_FILE`, `--gst-debug-help`, `-v`, `-m` | `bash gstreamer/12-debugging-and-tracing/01_debug_levels.sh` |
| [02_dot_graph.sh](02_dot_graph.sh) | dot dumps from gst-launch at every state change, renders PNG if graphviz is installed | `bash gstreamer/12-debugging-and-tracing/02_dot_graph.sh [outdir]` |
| [03_tracers.sh](03_tracers.sh) | latency, rusage, stats + `gst-stats-1.0`, leaks | `bash gstreamer/12-debugging-and-tracing/03_tracers.sh` |

Build requirements: `sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-tools` (and `graphviz` to render graphs).

## 9. Exercises

1. *(easy)* Run `01_debug_levels.sh`, then rerun step 2 with
   `videotestsrc:6`. How many more lines per frame do you get? Why is LOG
   "per buffer"?
2. *(easy)* Run `KEEP_DOT=1 ./build/gstreamer/12-debugging-and-tracing/02_dot_dump`,
   install graphviz, render the PNG. Find the negotiated format on the tee
   → queue links. *Hint:* edge labels.
3. *(medium)* Fix `04_refleak_bug.c` (copy it to `04_refleak_fixed.c`) and
   prove with the leaks tracer that nothing is alive.
4. *(medium)* In `03_tracers_in_code.c`, parse the `time=(guint64)N` field
   with `gst_structure_from_string` and print min/avg/max latency.
   *Hint:* strip the record to a structure string first.
5. *(medium)* Make a pipeline fail with `not-negotiated`
   (`videotestsrc ! video/x-raw,format=GRAY8 ! video/x-raw,format=RGB ! fakesink`
   with the second caps set *after* linking — see lesson 05). Dump a dot
   graph in the ERROR handler.
6. *(hard)* Build GstShark for your PC (meson), run
   `GST_TRACERS="interlatency" GST_DEBUG=GST_TRACER:7` on a 3-element
   pipeline with a `queue max-size-buffers=1` and an `identity sleep-time=10000`,
   and find the slow element from the numbers alone.
7. *(hard, board)* On the BeagleBone AI-64, run a v4l2 camera pipeline with
   `GST_DEBUG=2,v4l2src:5,GST_TRACER:7 GST_TRACERS=rusage` and identify the
   thread with the highest CPU.

## 10. Remember forever

```
GST_DEBUG=2,<cat-glob>:<0-9>      ERROR1 WARN2 FIXME3 INFO4 DEBUG5 LOG6 TRACE7 MEMDUMP9
  start at 2; glob one area (*CAPS*:5, v4l2src:5); GST_DEBUG_FILE, _NO_COLOR
GST_DEBUG_DUMP_DOT_DIR=dir  (before init)  -> dot -Tpng x.dot -o x.png
  in code: GST_DEBUG_BIN_TO_DOT_FILE(bin, GST_DEBUG_GRAPH_SHOW_ALL, "name")
GST_TRACERS="latency(flags=pipeline+element);leaks;stats;rusage" + GST_DEBUG=GST_TRACER:7
  leaks prints at gst_deinit(); stats -> gst-stats-1.0; interlatency = GstShark
gst-inspect-1.0 x  |  gst-launch-1.0 -v (caps) -m (messages) -e (EOS on ^C)
first ERROR/WARNING above "Internal data stream error" is the real cause
```

Review it with the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

## 11. Self-test questions

1. What do levels 2, 4, 5 and 6 add, and which one is "per buffer"?
2. Why must `GST_TRACERS` be set before `gst_init()`?
3. Which file name from gst-launch's dot dumps shows the negotiated caps, and why
   that one?
4. What are the two columns of a debug line that tell you the thread and the
   object, and how do you use them to see a queue's thread boundary?
5. Which tracer finds a forgotten `gst_object_unref(pad)`, and what must your
   program call for it to report?
6. You see `Internal data stream error ... not-negotiated`. List your first
   three debugging steps.
7. Where does `interlatency` come from, and what does it measure that
   `latency` does not?
8. Why can turning on `GST_DEBUG=*:6` make a race condition disappear?
