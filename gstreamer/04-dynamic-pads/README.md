# 04 — Dynamic pads: always, sometimes, request, and `pad-added`

> Prerequisites: [02-first-pipeline](../02-first-pipeline/README.md) (build, link, run),
> [03-bus-and-messages](../03-bus-and-messages/README.md) (EOS/ERROR on the bus).

## 1. Why this exists

In lesson 02 every element had its pads from birth, so you could link
everything in `main()` before pressing play. That stops working the moment
you open a **file or a network stream**: a `.mkv` or `.mp4` might hold one
video track, or video + 3 audio languages + subtitles. The demuxer only
knows **after it has read the header**, i.e. after the pipeline is running.
So its output pads are created *at run time*, and you link them *at run time*
in a callback.

Three related needs show up again and again:

| Need | Pad kind | Example |
|------|----------|---------|
| "This element always has an input and an output" | **ALWAYS** | `videoconvert`, `identity`, `jpegenc` |
| "I will tell you which outputs exist once I see the data" | **SOMETIMES** | `matroskademux`, `qtdemux`, `decodebin`, `rtpbin` |
| "Give me another output/input, I don't know how many you need" | **REQUEST** | `tee` (src_%u), `matroskamux` (video_%u), `compositor` (sink_%u), `funnel` |

If you get this lesson, the error `Internal data stream error ... not-linked`
will never confuse you again: it means "a sometimes pad appeared and nobody
linked it", or "you linked it too late".

## 2. Mental model

**Analogy — a post-office sorting room.**

* An **ALWAYS** pad is a fixed pipe welded to the machine: you connect your
  hose before switching on.
* A **SOMETIMES** pad is the sorting machine opening a *new chute* only when
  it sees the first parcel for, say, "Germany". You cannot attach a hose to a
  chute that does not exist yet; instead you ask the machine: *"ring a bell
  (`pad-added`) when you open a chute, and I'll attach a hose then"*.
* A **REQUEST** pad is a splitter with an unlimited supply of outlets that you
  ask for: *"give me another outlet"* (`gst_element_request_pad_simple`) and
  hand back when done (`gst_element_release_request_pad`).

```
   ALWAYS                  SOMETIMES                          REQUEST
  ┌──────────────┐        ┌────────────────────────┐        ┌──────────────┐
 ►│sink       src│►      ►│sink           video_%u │► ?     ►│sink     src_0│►
  └──────────────┘        │               audio_%u │► ?      │         src_1│►  (you asked)
   exists at creation     │            subtitle_%u │► ?      │         src_.│
                          └────────────────────────┘        └──────────────┘
                           appear after the header           appear when YOU ask
                           -> "pad-added" signal             -> request_pad_simple()
```

**Pad templates** are the blueprints. Every factory publishes them (see
`gst-inspect-1.0 matroskademux`, section *Pad Templates*):

```
  SRC template: 'video_%u'
    Availability: Sometimes           <- direction + presence + name pattern
    Capabilities: ANY                 <- what may flow (lesson 05)
```

`%u` / `%d` / `%s` in a template name means "numbered instances": `video_0`,
`video_1`... A pad is always created *from* a template.

## 3. How it works

### pad-added step by step (program 01)

```
 main thread                         demux streaming thread (filesrc pulls / pushes)
 ───────────                         ──────────────────────────────────────────────
 build: filesrc ─► demux  jpegdec ─► convert ─► sink
 connect "pad-added" -> on_pad_added
 set_state(PLAYING) ───────────────► demux reads EBML header, tracks
                                     creates pad "video_0" from template video_%u
                                     gst_element_add_pad() ─► emits "pad-added"
                                        └─► on_pad_added(demux, video_0, dec)  ← YOUR CODE,
                                              check caps: image/jpeg ?        IN THIS THREAD
                                              gst_pad_link(video_0, dec:sink)
                                     demux emits "no-more-pads" (all tracks found)
                                     pushes buffers through the new link ─► ...
 wait on bus ◄──────────────────────────────────────────────────────── EOS
```

Key facts:

1. **`pad-added` runs in a streaming thread**, not in your `main()`. Keep it
   short, never block on the bus in it, protect shared state.
2. The new pad is **transfer none**: you borrow it, don't unref it.
3. **Look at the caps** before linking. `gst_pad_get_current_caps()` may be
   `NULL` for a demuxer pad that has not pushed the CAPS event yet; then use
   `gst_pad_query_caps()`. Both return a ref you must `gst_caps_unref()`.
4. Elements downstream of the dynamic link are already in the pipeline and in
   the same state (we added them before PLAYING). If you add elements *inside*
   the callback, call `gst_element_sync_state_with_parent()` on them, or they
   stay in NULL and refuse data (`not-linked` / `flushing`).
5. `no-more-pads` signal: "I've created every pad I will". Useful to decide
   what to do with streams nobody linked.

### decodebin / uridecodebin / playbin

You rarely write the demux → parser → decoder chain by hand. `decodebin`
**autoplugs** it: it runs `typefind`, looks up factories in the registry whose
sink caps match, prefers the highest **rank**, and repeats until the output is
raw (`video/x-raw`, `audio/x-raw`).

```
                        ┌──────────────────────── uridecodebin ─────────────────────────┐
  uri=file:///a.mkv ──► │  source by URI scheme                                          │
                        │  file:// → filesrc, http:// → souphttpsrc, rtsp:// → rtspsrc   │
                        │        │                                                        │
                        │  ┌─────▼─────────────── decodebin ─────────────────────────┐   │
                        │  │ typefind ──caps?──► matroskademux ─video_0─► h264parse ─► decoder ─┼─► src_0 (video/x-raw)  ┄► pad-added
                        │  │  "this is          (registry lookup  └audio_0─► aacparse ─► decoder ─┼─► src_1 (audio/x-raw)  ┄► pad-added
                        │  │   video/x-matroska" by caps + rank)                    │   │
                        │  └─────────────────────────────────────────────────────────┘   │
                        └────────────────────────────────────────────────────────────────┘
```

* **decodebin**: you give it bytes on its sink pad.
* **uridecodebin**: you give it a URI; it adds the source too (program 02).
* **playbin**: uridecodebin **plus** the output side: audio/video sinks,
  converters, queues, volume, subtitles. You only set `uri` (program 04).
  Also gives you `n-video`, `n-audio`, `current-audio` (language switch) etc.
* Newer variants: `decodebin3`, `uridecodebin3`, `playbin3` (stream
  selection via GstStreamCollection). Same idea, better for many streams.

Autoplugging is also how **hardware decoders get picked**: on the BeagleBone
AI-64 with TI's SDK image the V4L2 hardware decoder elements are registered
with a rank that makes decodebin choose them over a software decoder
(verify with `gst-inspect-1.0 | grep v4l2` and the element's `Rank`). If
you need control, set rank via `GST_PLUGIN_FEATURE_RANK=name:MAX` env var or
build the chain explicitly.

### Request pads (program 03)

```
  GstPad *p = gst_element_request_pad_simple(tee, "src_%u");  // refcount 2: tee + you
  ... link p, use it ...
  gst_element_release_request_pad(tee, p);                     // tee drops its ref
  gst_object_unref(p);                                         // you drop yours
```

`gst_element_request_pad_simple` (GStreamer ≥ 1.20) replaces the deprecated
`gst_element_get_request_pad`. Request pads are not static, so
`gst_element_get_static_pad(tee, "src_0")` does **not** give them to you.

`gst_element_link(a, b)` will request pads automatically if needed (e.g.
linking into a muxer) — and parse-launch does the same. But when *you* tear a
branch down at runtime you must release what was requested (lesson 08).

## 4. Complexity table

| Operation | Cost | Why |
|-----------|------|-----|
| `pad-added` callback | once per new stream | a signal emission; cheap, but it blocks the demuxer's thread while it runs |
| `gst_pad_link` | O(1) + caps check | checks templates/caps compatibility (`GST_PAD_LINK_CHECK_*`); no data copied |
| decodebin autoplugging | registry lookups per stage, once per stream | typefind reads the first KBs; factory list is sorted by rank |
| playbin start-up | typefind + preroll | first frame must arrive at each sink before PAUSED completes |
| Request/release a tee pad | O(1) | adds a child pad; tee then pushes the **same buffer ref** to each pad (no copy) |
| Data through a dynamic link | identical to static | once linked, a pad is a pad: same zero-copy push path |

## 5. C vs C++

C uses `g_signal_connect(demux, "pad-added", G_CALLBACK(cb), user_data)` and a
plain function. In C++ the callback must still be a function with C linkage
shape (no capturing lambda can be passed as a `GCallback`). Common trick:
pass `this` as `user_data` and a static member function or captureless lambda
that forwards:

```cpp
g_signal_connect(demux, "pad-added",
    G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer self) {
        static_cast<Player*>(self)->on_pad_added(pad);
    }), this);
```

(The unary `+` turns a captureless lambda into a plain function pointer.)
Remember to `g_signal_handler_disconnect` or stop the pipeline before `this`
dies. Lesson 09 wraps this in RAII.

## 6. Where it is used in the real world

* **Every media player** (Totem, GNOME Videos, many embedded HMIs) is built on
  `playbin`/`playbin3`.
* **RTSP/RTP receivers** (`rtspsrc`, `rtpbin`) create pads per received
  stream (SSRC / payload type) → always pad-added.
* **Transcoders and recorders** use `uridecodebin` + encoders, linking
  audio and video branches in pad-added.
* **TDA4VM / BeagleBone AI-64**: Edge AI demo pipelines read MP4 files with
  `qtdemux` (sometimes pads) and send video to the hardware decoder; a camera
  over RTSP comes through `rtspsrc`, again with pad-added.
* `tee` request pads: record + display + AI inference on the same camera.

## 7. Common mistakes and pitfalls

1. **Linking a sometimes pad in `main()`** → link fails, then at play time
   `not-linked` error. Use pad-added.
2. **Ignoring the caps in pad-added** → linking the audio pad to a video
   decoder: `not-negotiated` or link refused. Always check the media type.
3. **Linking twice** when a file has two video tracks: check
   `gst_pad_is_linked(sinkpad)` first.
4. **Adding elements in the callback but not syncing their state** → they stay
   in NULL: `gst_element_sync_state_with_parent()`.
5. **Blocking in pad-added** (waiting on the bus, sleeping, taking a lock
   held by main while main waits for state change) → deadlock.
6. **Leaving streams unlinked**: with demuxers, a not-linked pad that
   receives data can stop the whole pipeline. Link a `fakesink` to streams
   you don't want, or use playbin / stream selection.
7. **Leaking request pads**: forget release *or* the unref → leak (the
   `leaks` tracer in lesson 12 will show `GstPad`).
8. **Using deprecated `gst_element_get_request_pad`** in new code (≥ 1.20 use
   `gst_element_request_pad_simple`).

## 8. Files in this lesson

| File | What it shows | Run |
|------|---------------|-----|
| [01_pad_added_demux.c](01_pad_added_demux.c) | writes an MJPEG `.mkv`, plays it back linking `matroskademux`'s `video_0` in `pad-added`, counts frames | `make run P=gstreamer/04-dynamic-pads/01_pad_added_demux.c` |
| [02_uridecodebin.c](02_uridecodebin.c) | `uridecodebin` autoplugs demux+decoder; we link only `video/x-raw` and pull RGB frames with appsink | `make run P=gstreamer/04-dynamic-pads/02_uridecodebin.c` |
| [03_pad_availability.c](03_pad_availability.c) | prints pad templates (ALWAYS/SOMETIMES/REQUEST) of 4 factories; request/release tee pads with refcount checks | `make run P=gstreamer/04-dynamic-pads/03_pad_availability.c` |
| [04_playbin.c](04_playbin.c) | `playbin` with fakesinks: prerolls, reads `n-video`/`n-audio`/duration, plays to EOS | `make run P=gstreamer/04-dynamic-pads/04_playbin.c` |

Add `--display` (after building) to see the video, e.g.
`./build/gstreamer/04-dynamic-pads/04_playbin --display`.
Build needs `sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev`.

Try on the command line too:

```sh
gst-inspect-1.0 matroskademux | sed -n '/Pad Templates/,/Element has no clocking/p'
gst-launch-1.0 -v uridecodebin uri=file:///path/to/file.mkv ! videoconvert ! autovideosink
gst-play-1.0 /path/to/file.mkv         # playbin with a tiny keyboard UI
```

## 9. Exercises

1. *(easy)* In 01, connect `no-more-pads` and print a message. In which
   thread does it run? (print `g_thread_self()`).
2. *(easy)* In 03, add `compositor` and `matroskamux` to the factories list.
   Which of their pads are REQUEST?
3. *(medium)* Write a file with **video and audio** (see the writer pipeline
   in 04) and extend 01 to link `audio_0` to `audioconvert ! fakesink`.
   Count video and audio buffers separately.
4. *(medium)* In 02, create the `videoconvert ! appsink` branch **inside**
   pad-added (not before PLAYING). What breaks if you forget
   `gst_element_sync_state_with_parent`?
5. *(hard)* Use `decodebin`'s `autoplug-select` signal to *refuse* `jpegdec`
   and observe the error. (Hint: the `GstAutoplugSelectResult` enum is not
   in a public header; its values are TRY=0, EXPOSE=1, SKIP=2 — return 2 for
   the jpegdec factory. Check with `gst-inspect-1.0 decodebin`.)
6. *(hard, board)* On the BeagleBone AI-64 run
   `gst-launch-1.0 -v filesrc location=x.mp4 ! qtdemux ! h264parse ! v4l2h264dec ! fakesink`
   (TI SDK image) and compare CPU load with `decodebin` picking a software
   decoder.

## 10. Remember forever

```
ALWAYS = exists at birth      -> link before PLAYING
SOMETIMES = appears with data -> g_signal_connect(e,"pad-added",cb); link in cb
REQUEST = you ask for it      -> request_pad_simple(e,"src_%u"); release + unref
pad-added runs in a STREAMING thread; pad is borrowed (transfer none)
In cb: get caps -> check media type -> is_linked? -> gst_pad_link
Elements added later: gst_element_sync_state_with_parent()
decodebin = typefind + autoplug by caps/rank until raw; playbin = + sinks
"not-linked" = a pad pushed data but nobody was connected
```

Review this card with the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

## 11. Self-test questions

1. Why can't you link `matroskademux` to `jpegdec` before going to PLAYING?
2. Which thread calls your `pad-added` handler, and what two things must you
   therefore avoid inside it?
3. What does `%u` in a pad-template name mean?
4. What is the refcount of a freshly requested tee pad, and who owns each ref?
5. How does decodebin decide between two decoders that both accept H.264?
6. What is the difference between `decodebin`, `uridecodebin` and `playbin`?
7. You get `streaming stopped, reason not-linked (-1)` — list two causes.
8. Why would you check `gst_pad_is_linked()` in pad-added?
