# 05 — Caps and negotiation: how elements agree on a format

> Prerequisites: [02-first-pipeline](../02-first-pipeline/README.md),
> [04-dynamic-pads](../04-dynamic-pads/README.md) (pad templates).

## 1. Why this exists

A buffer is just bytes. 230 400 bytes could be a 320×240 RGB frame, a
640×360 GRAY8 frame, or 1.3 s of 44.1 kHz stereo audio. Before any byte
flows, both ends of every link must **agree on what the bytes mean**: media
type, pixel format, width, height, frame rate, sample rate, channels...

That description is called **caps** (capabilities), and the agreement process
is **caps negotiation**. Get it wrong and you meet GStreamer's most famous
error: `streaming stopped, reason not-negotiated (-4)`.

On the TDA4VM this matters even more: the hardware blocks (VPAC scaler, codec,
display) only accept *specific* formats (typically NV12 and a few others) and
sizes/strides with alignment rules. Negotiating the right format up front is
the difference between zero-copy and a CPU `videoconvert` burning a core.

## 2. Mental model

**Analogy — plugs and sockets, then a contract.**

Each pad has a *socket shape* (its template caps: "I accept RGB or I420, any
size from 1 to 4096"). Linking two pads is checking the shapes *could* fit.
Negotiation is then a **contract** the two sides sign *before the first
delivery*: "it will be RGB, 640×240, 30 fps". The contract is sent downstream
as a **CAPS event**, and every buffer after it is interpreted by that contract
until a new one arrives.

```
  GstCaps  = list of GstStructure (alternatives, in order of preference)
  ┌──────────────────────────────────────────────────────────────────┐
  │ [0] video/x-raw                       ← media type (structure name)
  │       format    = { I420, RGB }       ← LIST   (unfixed)
  │       width     = [ 1, 4096 ]         ← RANGE  (unfixed)
  │       height    = [ 1, 4096 ]
  │       framerate = [ 0/1, 120/1 ]      ← fraction range
  │ [1] video/x-raw(memory:DMABuf)        ← caps FEATURE: same format, other memory
  │       format = NV12, ...
  └──────────────────────────────────────────────────────────────────┘
  FIXED caps  = exactly one structure, every field a single value.
  Only fixed caps can be put in a CAPS event (and describe a buffer).
```

Negotiation is **set algebra**:

```
   upstream can:   format={I420,RGB}, width=[1,4096]
 ∩ downstream can: format=RGB,        width=[320,1920]
 = common:         format=RGB,        width=[320,1920]     (intersect)
 → fixate:         format=RGB,        width=320            (pick one value)
   (or steered: fixate_field_nearest_int(width, 640) → 640)
 ∅ (empty)  → not-negotiated
```

## 3. How it works

### The negotiation sequence (push mode, video source → sink)

```
  videotestsrc:src                 capsfilter                  sink:sink
  ────────────────                 ──────────                  ─────────
  1. CAPS QUERY  ───────────────►  "what can you accept?"  ──►  answers template caps
                 ◄─────────────── intersect with filter caps ◄─ (result travels back)
  2. intersect own caps with the answer, then FIXATE
  3. (optional) ACCEPT_CAPS query ─► "will you take exactly this?"  ─► yes/no
  4. CAPS EVENT (fixed caps) ─────►  stored on each pad  ──────────►  sink configures itself
  5. ALLOCATION QUERY ───────────►  downstream proposes a buffer pool, size,
                 ◄──────────────    alignment, metas (GstVideoMeta for strides)
  6. buffers flow  ═══════════════════════════════════════════════════►
  7. later: RECONFIGURE event (upstream) when downstream wants other caps
          → source renegotiates and sends a new CAPS event
```

* A **query** goes and comes back with an answer (synchronous, any direction).
* An **event** goes one way and is serialized with data (CAPS, SEGMENT, EOS).
* Step 5 is where **zero-copy** is decided: if downstream (e.g. a hardware
  encoder or a DMA-capable display) offers its buffer pool, upstream writes
  directly into it (lesson 13).

### Who does what

| Element | Role in negotiation |
|---------|--------------------|
| `capsfilter` | Intersects whatever passes with its `caps` property. `a ! video/x-raw,width=640 ! b` in gst-launch is shorthand for a capsfilter. |
| `videoconvert` | Accepts any raw format in, offers any raw format out: bridges format mismatches (CPU cost!). |
| `videoscale` | Bridges size mismatches. |
| `videorate` | Bridges framerate mismatches (duplicates/drops frames). |
| `audioconvert` / `audioresample` | Same for audio format/channels and sample rate. |
| sources | Fixate: choose a value from ranges (videotestsrc defaults to 320×240 if free). |

### Strides and frame size (program 04)

Fixed caps → `GstVideoInfo` → **stride per plane, offset per plane, frame
size**. Never compute `width * bytes_per_pixel` yourself: GStreamer pads rows
to 4 bytes by default, and hardware often to 16, 64 or more.

```
  GRAY8, width=158                       stride = GST_ROUND_UP_4(158) = 160
  row 0 │Y Y Y ... Y│p p│
  row 1 │Y Y Y ... Y│p p│      pixel(x,y) = data[y * stride + x]   ✔
         ◄── 158 ──►◄2►         pixel(x,y) = data[y * width  + x]   ✘ drifts 2 bytes/row
```

Use `GstVideoFrame` (`gst_video_frame_map`) to get `PLANE_DATA` and
`PLANE_STRIDE`; it also honours a `GstVideoMeta` attached by hardware
elements with non-default strides.

### Color range trap

`GRAY8` defaults to *full range* (0..255), `I420`/`NV12` default to *limited
range* (Y 16..235). So "white" in I420 is Y=235, and RGB→GRAY8 uses
`Y = 0.299R + 0.587G + 0.114B` (see 06/05). If your AI model expects 0..255
and receives 16..235, accuracy quietly drops.

## 4. Complexity table

| Operation | Cost | Why |
|-----------|------|-----|
| `gst_caps_intersect` | O(n·m) structures × fields | pairwise intersection of every structure; caps with huge lists (e.g. all formats) are slow → cache, don't call per buffer |
| CAPS query | walks the downstream chain once | each element transforms and forwards; done at start and on reconfigure, not per buffer |
| CAPS event | O(1) per pad | stored as a sticky event on the pad |
| A buffer after negotiation | zero negotiation cost | caps are known; buffers carry no format |
| `videoconvert` | O(pixels) **per frame**, CPU | the price of a mismatch; avoid on embedded (use HW converters or negotiate the native format) |
| `videoscale` | O(output pixels × taps) per frame | bilinear by default |
| Renegotiation | a pipeline "hiccup" | pools are re-created; avoid changing caps often |

## 5. C vs C++

The C API is the same from C++. Caps are refcounted mini-objects, so in C++
wrap them:

```cpp
struct CapsUnref { void operator()(GstCaps* c) const { gst_caps_unref(c); } };
using CapsPtr = std::unique_ptr<GstCaps, CapsUnref>;
CapsPtr caps(gst_caps_from_string("video/x-raw,format=NV12"));
g_object_set(filter, "caps", caps.get(), nullptr);   // property takes its own ref
```

Watch transfer annotations: `gst_caps_fixate(caps)` **consumes** its argument
(transfer full) and returns new caps — in C++ write
`CapsPtr fixed(gst_caps_fixate(caps.release()));`.
`gst_caps_get_structure()` returns a borrowed pointer: don't wrap it.
In C++ `GstMapFlags`, `GstSeekFlags`, `GstMessageType` combinations need
`static_cast<GstMapFlags>(GST_MAP_READ | GST_MAP_WRITE)` because C++ forbids
implicit int → enum conversion.

## 6. Where it is used in the real world

* **Camera capture** (`v4l2src`): the camera driver lists supported
  formats/sizes/framerates (`v4l2-ctl --list-formats-ext`); your capsfilter
  picks one: `v4l2src ! video/x-raw,format=YUY2,width=1280,height=720,framerate=30/1`.
  On the BeagleBone AI-64 with a CSI camera through the TI CSI-RX/ISP path you
  typically ask for the sensor's Bayer/NV12 output and let the TI
  hardware elements convert; check each element's pads with `gst-inspect-1.0`.
* **Encoders** (`v4l2h264enc`, x264enc): accept only specific input formats;
  a capsfilter before them avoids an accidental software conversion.
* **Muxers**: refuse streams whose caps they can't store.
* **Neural-network inference** (TI's `tidlinferer`, or your appsink): the
  model wants e.g. RGB 224×224 — caps enforce it.
* **Audio**: `audio/x-raw,format=S16LE,rate=48000,channels=2` for ALSA
  devices that support only that.

## 7. Common mistakes and pitfalls

1. **`not-negotiated`**: two sides with no common caps (program 03). Fix by
   inserting `videoconvert`/`videoscale`/`videorate`/`audioconvert`/
   `audioresample`, or correct the capsfilter. Debug with `gst-launch-1.0 -v`
   (prints negotiated caps per pad) and `GST_DEBUG=GST_CAPS:5`.
2. **Link-time vs run-time failure**: `gst_element_link` checks template caps
   only; a conflicting capsfilter set later fails when data flows.
   parse-launch with incompatible fixed filters fails to *link* instead
   (`not-linked`).
3. **Computing frame size as `w*h*bpp`**: ignores stride padding (04).
4. **Forgetting framerate** in appsrc caps: sinks can't sync, encoders
   misbehave. `framerate=0/1` means "variable".
5. **Over-constraining**: forcing a format the hardware doesn't support
   inserts silent CPU conversions (when there is a `videoconvert`) — check
   with `-v`.
6. **Caps refcount**: `g_object_set(..., "caps", caps)` takes its own ref; you
   must still unref yours. `gst_pad_get_current_caps` returns a ref (unref
   it); `gst_caps_get_structure` does not.
7. **Writing to shared caps**: caps with refcount > 1 are read-only; use
   `gst_caps_make_writable()` (copies if shared).

## 8. Files in this lesson

| File | What it shows | Run |
|------|---------------|-----|
| [01_caps_basics.c](01_caps_basics.c) | parse, inspect fields/ranges, intersect, subset, fixate, copy-on-write, `gst_caps_new_simple` | `make run P=gstreamer/05-caps-and-negotiation/01_caps_basics.c` |
| [02_capsfilter_negotiation.c](02_capsfilter_negotiation.c) | capsfilter forces NV12 640×360@15; read negotiated caps, `GstVideoInfo` (size, planes), `notify::caps` | `make run P=gstreamer/05-caps-and-negotiation/02_capsfilter_negotiation.c` |
| [03_not_negotiated.c](03_not_negotiated.c) | provokes `not-negotiated`, recognises it on the bus, fixes it with `videoconvert` | `make run P=gstreamer/05-caps-and-negotiation/03_not_negotiated.c` |
| [04_convert_and_scale.c](04_convert_and_scale.c) | I420 640×480 → GRAY8 158×120 via videoconvert+videoscale; checks stride padding and every pixel with `GstVideoFrame` | `make run P=gstreamer/05-caps-and-negotiation/04_convert_and_scale.c` |

Programs 02 and 04 accept `--display`. Command-line experiments:

```sh
gst-launch-1.0 -v videotestsrc num-buffers=1 ! video/x-raw,format=NV12 ! fakesink   # see caps per pad
GST_DEBUG=GST_CAPS:4 gst-launch-1.0 videotestsrc num-buffers=1 ! fakesink 2>&1 | head
gst-inspect-1.0 videoconvert      # look at the formats list in its pad templates
```

## 9. Exercises

1. *(easy)* In 01, intersect `audio/x-raw,rate=[8000,48000]` with
   `audio/x-raw,rate={44100,96000}`. Predict the result first.
2. *(easy)* In 02, change NV12 to I420 and YUY2: how do `GST_VIDEO_INFO_SIZE`
   and `N_PLANES` change? Explain each.
3. *(medium)* Make 03 fail with a *size* conflict instead of a format conflict
   and fix it with `videoscale`.
4. *(medium)* Add a pad probe (lesson 07) or `notify::caps` on every pad of a
   `videotestsrc ! videoconvert ! videoscale ! capsfilter ! fakesink`
   pipeline and print the caps at each link. Where does the format change?
5. *(hard)* Write a program that changes the capsfilter's caps **while
   PLAYING** (e.g. width 320 → 640 every 30 frames) and count the CAPS events
   at the sink. That is renegotiation.
6. *(hard, board)* On the BeagleBone AI-64 list your camera's formats with
   `v4l2-ctl --list-formats-ext` and write the capsfilter that selects the
   largest NV12/YUYV mode at ≥ 30 fps.

## 10. Remember forever

```
caps = media type + fields; list {a,b}, range [lo,hi] = unfixed; one value each = fixed
negotiate = CAPS query (ask) -> intersect -> fixate -> CAPS event (tell) -> ALLOCATION query
empty intersection => "not-negotiated (-4)"; fix with convert/scale/rate or a correct capsfilter
"a ! video/x-raw,... ! b" is a capsfilter
fixed caps -> GstVideoInfo -> stride/offset/size; NEVER w*h*bpp; map with GstVideoFrame
gst-launch-1.0 -v shows the negotiated caps on every pad
get_current_caps/query_caps return refs (unref); get_structure is borrowed
```

Review this card with the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

## 11. Self-test questions

1. What makes caps "fixed"? Why must a CAPS event carry fixed caps?
2. Walk through the 5 negotiation steps from source to sink, naming each
   query/event and its direction.
3. What is the result of intersecting `format={I420,RGB}` with
   `format=GRAY8`, and what error does it cause at run time?
4. Why may the buffer for a 158-pixel-wide GRAY8 frame be 160 × height bytes?
5. Which elements would you insert to fix format, size, framerate and
   sample-rate mismatches?
6. What is a caps *feature* such as `memory:DMABuf`, and why does it matter on
   the TDA4VM?
7. Why does `gst_element_link` succeed with a capsfilter whose caps are set
   later, but the pipeline fails when playing?
8. What is the ALLOCATION query for?
