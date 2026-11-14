# 10 — Writing your own element (and plugin)

> Prerequisites: lessons 02 (pipeline skeleton), 05 (caps), 06 (appsink).
> Dev headers: `sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev`

## 1. Why this exists

Sooner or later the element you need does not exist: a custom overlay, a
sensor-specific pixel fix, a statistics probe that must sit *inside* the
pipeline, a wrapper around your company's algorithm or an accelerator API.
You could do it in an appsink → your code → appsrc sandwich (lesson 06), but
then you own the threads, the timestamps, the caps and the copies.

Writing an element lets your code become a normal brick: usable from
`gst-launch-1.0`, from Python, from `playbin`, reusable by colleagues,
inspectable with `gst-inspect-1.0`, and free of extra copies, because it runs
in the streaming thread on the buffer that is already there.

The good news: you almost never write an element from zero. GStreamer ships
**base classes** (`GstBaseTransform`, `GstBaseSrc`/`GstPushSrc`,
`GstBaseSink`, `GstVideoFilter`, `GstAggregator`, `GstVideoDecoder`...) that
already implement 90 % of the hard parts (pads, events, queries, states,
locking). You fill in a few virtual functions.

## 2. Mental model

**An element is a workstation on the factory line; a base class is a
pre-built workstation with only the "do the work" slot left empty.**

```
  Factory line analogy
  ────────────────────
     conveyor in                                            conveyor out
  ───────────────►┌──────────────────────────────────────┐───────────────►
     (sink pad)   │  PRE-BUILT STATION (GstBaseTransform) │  (src pad)
                  │  ✔ agrees the part type   (caps)      │
                  │  ✔ passes on notices      (events)    │
                  │  ✔ answers questions      (queries)   │
                  │  ✔ starts/stops w/ shift  (states)    │
                  │  ✔ gets a writable part   (make_writable)
                  │  ┌────────────────────────────────┐  │
                  │  │  YOUR SLOT: transform_ip()     │  │
                  │  │  buf[i] = 255 - buf[i]         │  │
                  │  └────────────────────────────────┘  │
                  └──────────────────────────────────────┘
```

And a **plugin** is the *crate* that ships one or more workstation designs
(element *factories*) to the factory. GStreamer opens the crate (dlopen of a
`.so`), reads the label (`GST_PLUGIN_DEFINE`), and calls `plugin_init()`,
which registers each factory by name.

```
  libgstmyplugin.so ──dlopen──► plugin_init() ──► gst_element_register("myinvert", TYPE)
                                                └► gst_element_register("mycounter", TYPE)
  later:  gst_element_factory_make("myinvert") ─► g_object_new(TYPE) ─► your _init()
```

In this lesson the crate is not a `.so`: `gst_plugin_register_static()`
registers the plugin from inside the program, so each lesson stays one file.

## 3. How it works

### 3.1 The GObject boilerplate, decoded

GStreamer elements are GObjects: C structs with a class (vtable) struct and
a runtime type ID. Two macros write almost all the boilerplate:

```c
#define MY_TYPE_INVERT (my_invert_get_type())
G_DECLARE_FINAL_TYPE(MyInvert, my_invert, MY, INVERT, GstBaseTransform)
//                   │         │          │   │       └ parent type
//                   │         │          │   └ type name UPPER  -> MY_INVERT()
//                   │         │          └ namespace UPPER       -> MY_IS_INVERT()
//                   │         └ function prefix -> my_invert_get_type()
//                   └ CamelCase struct name

struct _MyInvert {             // instance struct: one per element object
    GstBaseTransform parent;   // FIRST member = "inherits" from parent
    gboolean invert;           // your state
};

G_DEFINE_TYPE(MyInvert, my_invert, GST_TYPE_BASE_TRANSFORM)
// writes my_invert_get_type() + my_invert_parent_class,
// and requires you to write:
static void my_invert_class_init(MyInvertClass *klass); // ONCE per type
static void my_invert_init(MyInvert *self);             // per instance
```

Memory picture of one instance (why "parent first" matters):

```
  MyInvert *self  ──►┌──────────────────────────┐ ◄── also a valid GstBaseTransform*
                     │ GObject  (refcount, type)│ ◄── also a valid GstElement*, GObject*
                     │ GstObject (name, lock)   │
                     │ GstElement (pads, state) │
                     │ GstBaseTransform (...)   │
                     ├──────────────────────────┤
                     │ invert, info, frames     │ ◄── your fields
                     └──────────────────────────┘
  Casting = same pointer, different view.  MY_INVERT(obj) checks the type too.
```

Use `G_DECLARE_DERIVABLE_TYPE` + a `MyInvertClass` struct if others should
subclass you. For elements, FINAL is the usual choice.

### 3.2 class_init: describe the element once

```
  class_init(klass)
    ├─ gobject_class->set_property / get_property      (property plumbing)
    ├─ g_object_class_install_property(... "invert" ...)
    ├─ gst_element_class_add_static_pad_template(sink) (what CAN flow in)
    ├─ gst_element_class_add_static_pad_template(src)  (what CAN flow out)
    ├─ gst_element_class_set_static_metadata(long name, KLASS, desc, author)
    └─ base-class vfuncs:  set_caps, transform_ip, (transform, transform_caps,
                           start, stop, sink_event, propose_allocation, ...)
```

The **klass string** (`"Filter/Effect/Video"`, `"Codec/Decoder/Video"`,
`"Source/Video"`) is not decoration: autopluggers such as decodebin search
factories by it, together with the **rank** passed to
`gst_element_register()` (`GST_RANK_NONE` = never auto-plugged,
`GST_RANK_PRIMARY` = preferred). Hardware decoders on the TDA4VM win over
software ones because of their rank.

### 3.3 Pad templates

```c
static GstStaticPadTemplate sink_tmpl = GST_STATIC_PAD_TEMPLATE(
    "sink",               // name (or "sink_%u" for request/sometimes pads)
    GST_PAD_SINK,         // direction
    GST_PAD_ALWAYS,       // availability: ALWAYS / SOMETIMES / REQUEST
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE("GRAY8")));
```

The template caps are the *outer limit*. During negotiation they are
intersected with the neighbours' caps (lesson 05). `GST_VIDEO_CAPS_MAKE("GRAY8")`
expands to `video/x-raw, format=GRAY8, width=[1,MAX], height=[1,MAX], framerate=[0/1,MAX]`.

### 3.4 GstBaseTransform: which vfunc runs when

```
  buffer arrives on sink pad (streaming thread)
         │
         ▼
  caps changed? ──yes──► transform_caps() / fixate_caps() ─► set_caps(in, out)
         │
         ▼
  passthrough? ──yes──► (transform_ip_on_passthrough ? transform_ip : nothing)
         │                          └─► push SAME buffer
         no
         ▼
  in-place? (only transform_ip set, or set_in_place(TRUE))
     ├─yes─► make buffer writable (copy ONLY if shared) ─► transform_ip(buf)
     └─no──► allocate outbuf (pool/size from caps) ─► transform(inbuf, outbuf)
         │
         ▼
  push to src pad
```

| vfunc | you implement it when | example |
|---|---|---|
| `transform_ip` | output = modified input, same size | invert, gain, overlay, draw boxes |
| `transform` | output is a *different* buffer (size or format change) | colour conversion, scaling |
| `transform_caps` | output caps differ from input caps | RGB in → GRAY8 out |
| `set_caps` | you must know width/height/stride/format | parse `GstVideoInfo` once |
| `start` / `stop` | open/close resources (files, devices, contexts) | allocate a LUT |
| `sink_event` | react to EOS, flush, custom events | flush internal state |
| `propose_allocation` / `decide_allocation` | zero-copy, pools, metas | lesson 13 |

**passthrough** (`gst_base_transform_set_passthrough()`): "don't touch,
just forward the same buffer". Our `invert=false` uses it: zero cost.
**Pitfall found while writing 01:** `transform_ip_on_passthrough` defaults
to **TRUE**, so BaseTransform *still calls* `transform_ip` in passthrough
mode (designed for read-only analysis elements). A modifying filter must set
it to FALSE in `class_init`, or it keeps modifying buffers it was told to
leave alone — possibly shared, read-only ones.

### 3.5 GstVideoFilter: the easier video-only option

`GstVideoFilter` derives from `GstBaseTransform` and does the
`GstVideoInfo` parsing and `gst_video_frame_map()` for you:

```c
G_DEFINE_TYPE(MyInvert, my_invert, GST_TYPE_VIDEO_FILTER)
static GstFlowReturn my_invert_transform_frame_ip(GstVideoFilter *f,
                                                  GstVideoFrame *frame)
{
    guint8 *p = GST_VIDEO_FRAME_PLANE_DATA(frame, 0);
    int stride = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);
    ... same loop ...
}
// class_init: GST_VIDEO_FILTER_CLASS(klass)->transform_frame_ip = ...;
```

Program 01 uses plain `GstBaseTransform` on purpose so you see the
`set_caps` + map/unmap steps that `GstVideoFilter` hides.

### 3.6 Properties

```
 g_object_set(e, "invert", FALSE, NULL)          (app thread)
        │  GObject looks up the GParamSpec "invert" (installed in class_init)
        │  validates the type (boolean) and range
        ▼
 my_invert_set_property(obj, PROP_INVERT, value)
        │  GST_OBJECT_LOCK  — the streaming thread may read it right now
        ▼
 self->invert = FALSE;  set_passthrough(TRUE)
```

Flags worth knowing: `G_PARAM_STATIC_STRINGS` (don't copy the name strings),
`GST_PARAM_MUTABLE_PLAYING` (documented as safe to change while playing),
`GST_PARAM_CONTROLLABLE` (can be animated by `GstController`).

### 3.7 Sources: GstBaseSrc / GstPushSrc (program 02)

```
  streaming thread loop (owned by GstBaseSrc):
     ┌──────────────────────────────────────────────────────────┐
     │ first time: negotiate                                     │
     │   template caps ∩ downstream (query) ─► fixate() ─► set_caps()
     │ loop:                                                     │
     │   create(&buf)   ◄── YOU: allocate, fill, set PTS/DURATION│
     │   push(buf)                                               │
     │   num-buffers reached? ─► push EOS event, pause task      │
     └──────────────────────────────────────────────────────────┘
```

* `gst_base_src_set_format(src, GST_FORMAT_TIME)` — video/audio sources
  speak time; the default is bytes (for file-like sources).
* `gst_base_src_set_live(src, TRUE)` — a camera: produces in real time, no
  preroll, `NO_PREROLL` on PAUSED. Program 02 is not live.
* `fixate()` turns ranges into single values. Program 02 proves that a
  downstream capsfilter still wins (`width=32`), and that a range downstream
  (`width=[100,200]`) picks the value nearest to the default.
* PTS of frame *n* = `gst_util_uint64_scale(n, GST_SECOND * fps_d, fps_n)`.
  **Never** accumulate `pts += duration`: at 30000/1001 fps the rounding error
  drifts.

### 3.8 Registering: static vs dynamic plugin

```c
// (A) inside a program (this lesson):
gst_plugin_register_static(GST_VERSION_MAJOR, GST_VERSION_MINOR,
    "myinvert", "description", plugin_init,
    "1.0", "LGPL", "source-module", "package", "origin-url");

// (B) in a shared library: one macro at the bottom of the file
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR,
    myinvert,                 // plugin name, NO quotes; .so must be libgstmyinvert.so
    "Course invert filter", plugin_init,
    "1.0", "LGPL", "course", "https://example.invalid/")
```

`GST_PLUGIN_DEFINE` exports a symbol `gst_plugin_myinvert_get_desc` that
the registry finds after `dlopen()`. The **license must be a known one**
(`"LGPL"`, `"GPL"`, `"MIT/X11"`, `"BSD"`, `"Proprietary"`...); an unknown
string makes the plugin be rejected.

### 3.9 Building it as a real .so plugin with meson

Layout:

```
gst-myinvert/
├── meson.build
└── src/
    └── gstmyinvert.c     the element part of 01_invert_filter.c (no main()),
                          plus GST_PLUGIN_DEFINE(...) at the end
```

`meson.build`:

```meson
project('gst-myinvert', 'c',
  version : '1.0.0',
  default_options : ['c_std=c11', 'warning_level=3', 'buildtype=debugoptimized'])

gst_dep       = dependency('gstreamer-1.0',       version : '>=1.20')
gstbase_dep   = dependency('gstreamer-base-1.0')
gstvideo_dep  = dependency('gstreamer-video-1.0')

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')

cdata = configuration_data()
cdata.set_quoted('PACKAGE',         'gst-myinvert')
cdata.set_quoted('VERSION',         meson.project_version())
cdata.set_quoted('GST_LICENSE',     'LGPL')
cdata.set_quoted('GST_PACKAGE_NAME','GStreamer course plugin')
cdata.set_quoted('GST_PACKAGE_ORIGIN', 'https://example.invalid')
configure_file(output : 'config.h', configuration : cdata)

library('gstmyinvert',                 # -> libgstmyinvert.so
  'src/gstmyinvert.c',
  c_args : ['-DHAVE_CONFIG_H'],
  include_directories : include_directories('.'),
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep],
  install : true,
  install_dir : plugins_install_dir)
```

Build, test without installing, then install:

```bash
meson setup build && ninja -C build
GST_PLUGIN_PATH=$PWD/build gst-inspect-1.0 myinvert        # metadata, pads, props
GST_PLUGIN_PATH=$PWD/build gst-launch-1.0 videotestsrc num-buffers=100 \
    ! video/x-raw,format=GRAY8 ! myinvert ! videoconvert ! autovideosink
sudo ninja -C build install    # -> /usr/local/lib/<triplet>/gstreamer-1.0/
```

If `gst-inspect-1.0 myinvert` says "No such element", run
`gst-inspect-1.0 -b` (lists **blacklisted** plugins: usually an undefined
symbol or a missing library), and remove the cache
`~/.cache/gstreamer-1.0/registry.*.bin` if you replaced the file.

**On the BeagleBone AI-64** (aarch64): cross-compile with a meson cross file
(lesson 14), copy `libgstmyinvert.so` to
`/usr/lib/aarch64-linux-gnu/gstreamer-1.0/` on the board (or anywhere listed
in `GST_PLUGIN_PATH`), then `gst-inspect-1.0 myinvert` on the board.

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| `transform_ip` on a GRAY8 frame | O(w·h) reads+writes, 0 allocations | works on the buffer already there |
| in-place on a *shared* buffer | + 1 full frame copy | BaseTransform calls `make_writable`; refcount > 1 forces a copy (e.g. after a `tee`) |
| `transform` (not in-place) | 1 output buffer per frame (from a pool if negotiated) + O(w·h) | input and output are different memory |
| passthrough | O(1) | same buffer pushed on; no map, no copy |
| `set_caps` | O(1), once per caps change | parse `GstVideoInfo` here, never per frame |
| property set | O(1) + lock | object lock contends with the streaming thread briefly |
| plugin load (.so) | dlopen once; registry cached | `~/.cache/gstreamer-1.0/registry.*.bin` avoids rescanning |
| threads added by the element | 0 for a transform, 1 for a source | a transform runs in the upstream thread; a `GstBaseSrc` owns a task |

## 5. C vs C++

* GStreamer elements are written in **C** (or Rust with `gstreamer-rs`,
  which is now popular for new plugins). The GObject type system *is* the
  object model; C++ classes cannot be registered as GObject types directly.
* In C++ you can still write elements: keep the `G_DECLARE_*`/`G_DEFINE_TYPE`
  boilerplate in an `extern "C"`-style section and call into a C++ object
  stored as a pointer in the instance struct (create it in `start()`, delete
  it in `stop()` — not in `init`/`finalize` if it holds stream resources).
  You will need `static_cast` for flag enums such as
  `static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)`.
* Using elements from C++ (lesson 09) is where RAII shines.

## 6. Where it is used in the real world

* **gst-plugins-good/bad** are hundreds of exactly these elements; read
  `videoflip`, `videobalance` or `identity` as clean examples.
* **TI Edge AI on the TDA4VM**: `tiovxmultiscaler`, `tiovxdlcolorconvert`,
  `tidlinferer` are custom elements of this kind (TI's plugins wrap OpenVX
  / TIDL behind GstBaseTransform/GstAggregator-style classes). Your own
  post-processing (draw boxes, filter detections) is typically one more
  in-place transform after `tidlinferer`.
* **NVIDIA DeepStream**, **Intel DL Streamer**: inference as elements.
* Camera bring-up: a tiny element to fix sensor quirks (black-level, bad
  pixel, Bayer pattern) before the ISP, testable with `videotestsrc`.

## 7. Common mistakes and pitfalls

1. **`transform_ip_on_passthrough` left TRUE** in a modifying filter: it
   keeps modifying in passthrough (found and fixed in program 01).
2. **Assuming `stride == width`**. Rows can be padded (GRAY8 rounds to 4
   bytes; hardware often to 64 or 128). Always use the stride from
   `GstVideoFrame` / `GstVideoInfo`.
3. **Parsing caps per buffer** instead of in `set_caps`.
4. **Forgetting the object lock** around properties read by the streaming
   thread.
5. **Unknown license string** or plugin name mismatch with the `.so`
   filename (`libgst<name>.so`) → plugin silently not loaded; check
   `gst-inspect-1.0 -b`.
6. **Allocating in `init()`** resources that belong to a stream: use
   `start()`/`stop()`, so the element can go NULL→PLAYING→NULL→PLAYING.
7. **No timestamps in a source**: sinks cannot sync, muxers fail.
   Remember `gst_base_src_set_format(GST_FORMAT_TIME)` too.
8. **Video "white" is 235**: `videotestsrc` produces limited-range video
   (16..235). Program 01 first measures the source instead of assuming 255.
9. Registering with `GST_RANK_PRIMARY` by accident makes decodebin pick
   your element automatically.

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [01_invert_filter.c](01_invert_filter.c) | GstBaseTransform in-place GRAY8 inverter, `invert` property + passthrough, static registration, pixel checks | `make run P=gstreamer/10-custom-element-plugin/01_invert_filter.c` |
| [02_counter_source.c](02_counter_source.c) | GstPushSrc source: caps ranges, `fixate()`, timestamps, frame numbers checked in appsink | `make run P=gstreamer/10-custom-element-plugin/02_counter_source.c` |

Both accept `--display` (run the binary from `build/...`) to show the
element in a window via `autovideosink`.

## 9. Exercises

1. *(easy)* Add an integer property `offset` (0–255) to `myinvert` that is
   added (with clamping) instead of inverting. Check with appsink.
2. *(easy)* Make `mycounter` live (`gst_base_src_set_live(TRUE)`) and
   observe the `NO_PREROLL` return of `set_state(PAUSED)`.
3. *(medium)* Rewrite `myinvert` on `GstVideoFilter` and accept GRAY8 **and**
   I420 (invert only the Y plane). Hint: `transform_frame_ip`, a list in the
   pad template caps: `GST_VIDEO_CAPS_MAKE("{ GRAY8, I420 }")`.
4. *(medium)* Turn `myinvert` into a real `.so` with the meson file above;
   run it from `gst-launch-1.0` with `GST_PLUGIN_PATH`.
5. *(hard)* Write an RGB → GRAY8 element with `transform` +
   `transform_caps` (output caps differ from input caps). Hint: in
   `transform_caps` replace the `format` field for the opposite direction.
6. *(hard)* Cross-compile the meson plugin for aarch64 and load it on the
   BeagleBone AI-64 after a `v4l2src` camera.

## 10. Remember forever

Review with the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

```
ELEMENT = GObject subclass of a base class; you fill the empty slot.
  G_DECLARE_FINAL_TYPE + G_DEFINE_TYPE  -> class_init (once) + init (each)
  class_init: pad templates, metadata(klass!), properties, vfuncs
Filter : GstBaseTransform  transform_ip (same buf) | transform (new buf)
         set_caps once; passthrough = forward same buffer;
         set transform_ip_on_passthrough = FALSE if you modify data
Source : GstPushSrc create(): alloc, fill, PTS = scale(n, SECOND*d, n_fps)
         set_format(TIME); fixate() picks values from ranges
Plugin : plugin_init -> gst_element_register(name, rank, TYPE)
         .so: GST_PLUGIN_DEFINE, libgst<name>.so, GST_PLUGIN_PATH, gst-inspect -b
```

## 11. Self-test questions

1. What do `G_DECLARE_FINAL_TYPE` and `G_DEFINE_TYPE` each generate?
2. Why must the parent struct be the first member of the instance struct?
3. What is the difference between `transform_ip` and `transform`, and when
   does in-place still cost a full copy?
4. What does passthrough do, and what is the trap with
   `transform_ip_on_passthrough`?
5. Why parse caps in `set_caps` and not in `transform_ip`?
6. What do the klass string and the rank control?
7. How does `fixate()` interact with a downstream capsfilter?
8. Why compute PTS from the frame number instead of adding durations?
9. Name three reasons a freshly built `.so` plugin is not found, and the
   command that shows blacklisted plugins.
10. Where would you install a plugin on the BeagleBone AI-64?
