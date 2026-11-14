# 09 — C++ RAII wrapper: never leak a GStreamer reference again

## 1. Why this exists

GStreamer is written in C on top of GObject. Every element, pad, bus, caps,
message, buffer and sample is **reference counted**, and every API function
says whether it gives you a reference you must drop (*transfer full*), lends
you one (*transfer none*), or hands you a *floating* one. Get one of these
wrong and you either:

- **leak** (forgot an unref): the object, its threads, its buffer pool and
  maybe a whole camera device stay alive; a long-running app on a BeagleBone
  slowly eats its RAM, or `/dev/video0` stays "busy" after you "closed" it;
- **crash** (unreffed something you did not own): use-after-free, often far
  away from the real bug, sometimes minutes later.

This is the #1 source of bugs in GStreamer applications. This lesson makes the
rules visible (by printing reference counts) and then makes them *automatic*:
`std::unique_ptr` with custom deleters and a small RAII `Pipeline` class in
C++, and `g_autoptr` in C.

## 2. Mental model

**A reference is a library card for a shared book.** The book (object) stays
on the shelf as long as at least one card is checked out. `ref` = take a card,
`unref` = return it. When the last card comes back, the book is destroyed
(finalized).

```
   GstElement "src"          refcount = number of cards out
   ┌──────────────┐
   │ refcount: 2  │◄──── card #1: held by the pipeline (its child)
   │ floating: no │◄──── card #2: held by YOU (gst_bin_get_by_name)
   └──────────────┘
   you forget to return card #2  →  pipeline dies, book stays forever  =  LEAK
   you return a card you never had → book destroyed while pipeline reads it = CRASH
```

A **floating** reference is a card nobody has signed yet. `gst_element_factory_make`
returns one. The first container that adopts the object (`gst_bin_add`, or
you with `gst_object_ref_sink`) signs it: the count stays 1, but now *that
container* owns it. That is why this works without any unref:

```c
GstElement *e = gst_element_factory_make("queue", NULL); // refcount 1, floating
gst_bin_add(GST_BIN(pipeline), e);                       // refcount 1, owned by bin
/* do NOT gst_object_unref(e) — that card belongs to the bin now */
```

RAII in one picture: a `unique_ptr` is a card that returns itself when the
variable goes out of scope.

```
 {                                     scope
   PadPtr pad(get_static_pad(...));    take card  (+1)
   if (error) return;                  ─┐
   throw std::runtime_error(...);      ─┤  every exit path runs ~PadPtr()
 }                                     ─┘  → gst_object_unref(pad)  (−1)
```

## 3. How it works

### The refcount rules (02_refcount_mistakes prints each one)

| Call                                         | What you get                   | You must                         |
|----------------------------------------------|--------------------------------|----------------------------------|
| `gst_element_factory_make`, `gst_pipeline_new`, `gst_bin_new` | **floating** ref          | give it to a bin, or `gst_object_ref_sink` then unref |
| `gst_bin_add(bin, e)`                        | bin sinks the floating ref     | nothing (do *not* unref)         |
| `gst_parse_launch`                           | full ref (already sunk)        | set NULL, then `gst_object_unref` |
| `gst_element_get_static_pad`, `gst_element_get_bus`, `gst_bin_get_by_name`, `gst_pad_get_peer`, `gst_element_request_pad_simple` | **full** ref (+1)  | `gst_object_unref` |
| `gst_pad_get_current_caps`, `gst_caps_from_string`, `gst_caps_new_*` | full caps ref | `gst_caps_unref` |
| `gst_bus_pop`, `gst_bus_timed_pop_filtered`  | full message ref               | `gst_message_unref`              |
| `gst_app_sink_pull_sample`                   | full sample ref                | `gst_sample_unref`               |
| `gst_sample_get_buffer`, `gst_sample_get_caps`, `gst_caps_get_structure`, `GST_MESSAGE_SRC` | **borrowed** (transfer none) | nothing; valid while the parent lives |
| `g_object_set(obj, "caps", caps, NULL)`      | setter takes its *own* ref     | still unref *your* caps          |
| `g_object_get(obj, "caps", &caps, NULL)`     | full ref / copy                | unref / `g_free` for strings     |
| `gst_pad_push`, `gst_pad_send_event`, `gst_element_post_message`, `gst_app_src_push_buffer` | **takes** your ref | do not touch or unref afterwards |
| `gst_message_parse_error(m, &err, &dbg)`     | newly allocated GError + string| `g_error_free`, `g_free`         |

How to know? The API documentation marks every parameter and return value with
`(transfer full)`, `(transfer none)` or `(transfer floating)`. Read it every
time you meet a new function.

Rule of thumb: **`get` / `new` / `make` / `pop` / `pull` / `request` returns a
ref you own. `parse` gives you strings you free. Functions that *send* or
*push* take your ref away.**

The leak proof in 02, with a weak pointer that GObject clears on finalize:

```
  src with our extra ref                               refcount=2
  after pipeline unref: src STILL ALIVE -> LEAK (refcount=1)
  after the fix (unref):  src finalized
```

### The C++ problem with flag enums

C lets you pass `GST_MESSAGE_EOS | GST_MESSAGE_ERROR` to a `GstMessageType`
parameter. In C++, `enum | enum` produces an `int`, and C++ does **not**
convert `int` back to an enum implicitly, so this does not compile:

```cpp
gst_bus_timed_pop_filtered(bus, t, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);   // error
gst_bus_timed_pop_filtered(bus, t,
    static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));      // OK
```

The same applies to every GStreamer flag type: `GstPadProbeType`,
`GstMapFlags` (`GST_MAP_READ | GST_MAP_WRITE`), `GstSeekFlags`
(`GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT`), `GstBufferFlags`,
`GstStateChange`... Write the `static_cast` once in a helper and reuse it.

### unique_ptr with custom deleters (01_unique_ptr_deleters)

```cpp
struct GstObjectDeleter { void operator()(gpointer p) const { gst_object_unref(p); } };
using ElementPtr = std::unique_ptr<GstElement, GstObjectDeleter>;
using PadPtr     = std::unique_ptr<GstPad,     GstObjectDeleter>;
struct GstMessageDeleter { void operator()(GstMessage *m) const { gst_message_unref(m); } };
using MessagePtr = std::unique_ptr<GstMessage, GstMessageDeleter>;

struct PipelineDeleter {             // encodes "NULL before unref"
    void operator()(GstElement *p) const {
        gst_element_set_state(p, GST_STATE_NULL);
        gst_object_unref(p);
    }
};
```

- A *stateless struct* deleter costs nothing: `sizeof(ElementPtr) ==
  sizeof(GstElement*)` (the program `static_assert`s it). A function-pointer
  deleter (`unique_ptr<T, void(*)(T*)>`) doubles the size.
- Wrap the raw pointer **on the same line** you receive it. Pass `.get()` to C
  APIs that borrow. Call `.release()` only for APIs that *take* the ref
  (e.g. `gst_bin_add(bin, elem.release())` if you had sunk it).
- Locals are destroyed in reverse order of declaration: declare the pipeline
  first, so it is destroyed last (after the pads/bus you got from it).

Why `unique_ptr` and not `shared_ptr`? GObject already counts references;
`shared_ptr` would add a second, independent count on top. If you need another
owner, call `gst_object_ref()` and wrap the result in a second `unique_ptr`.

### An RAII Pipeline class (03_pipeline_class)

```
 class Pipeline                                   (move-only)
 ├─ ctor(desc)    gst_parse_launch; throws if NULL OR if GError set
 │                (a missing element returns a partial pipeline + error!)
 ├─ ~Pipeline()   set_state(NULL); gst_object_unref
 ├─ copy          = delete     (two owners of one ref → double unref)
 ├─ move          std::exchange(o.pipe_, nullptr)  (source left empty)
 ├─ on(type, fn)  std::map<GstMessageType, std::function<void(GstMessage*)>>
 ├─ onSample(name, fn)  appsink new_sample → captureless lambda → std::function
 └─ run()         PLAYING, bus loop with MessagePtr, returns true on EOS
```

Two C-callback tricks are worth remembering:

1. A C callback must be a plain function pointer. A **captureless** lambda
   converts to one; a lambda with captures does not. So the C callback is a
   captureless lambda that casts `user_data` back to your C++ object and calls
   it.
2. The `user_data` pointer must stay valid as long as the C side can call it.
   The `std::function` lives in a `unique_ptr` stored in the class, so its
   address survives even when the `Pipeline` itself is moved.

### g_autoptr: RAII in C (04_g_autoptr)

```c
g_autoptr(GstElement) pipeline = gst_parse_launch(desc, &err);  /* unref at '}' */
g_autoptr(GstBus) bus = gst_element_get_bus(pipeline);
g_autoptr(GError) err = NULL;
g_autofree gchar *s = gst_caps_to_string(caps);
return g_steal_pointer(&pipeline);    /* hand ownership out, local becomes NULL */
```

This uses the GCC/Clang `cleanup` attribute — not ISO C, but every embedded
Linux toolchain (including `aarch64-linux-gnu-gcc` for the BeagleBone) has it.
GStreamer itself uses it extensively. It unrefs; it does **not** set the
pipeline to NULL — that is still your job.

## 4. Complexity table

| Operation                        | Cost                                  | Why |
|----------------------------------|---------------------------------------|-----|
| `gst_object_ref` / `unref`       | O(1), one atomic increment/decrement  | lock-free atomic on the refcount |
| final unref (finalize)           | O(size of object): frees pads, children, pools | recursive dispose/finalize |
| `unique_ptr` with struct deleter | zero space, zero time overhead        | deleter inlined, no stored pointer |
| `unique_ptr` with fn-ptr deleter | +1 pointer per object                 | the function pointer is stored |
| `std::function` handler call     | one indirect call (maybe a heap alloc at creation) | type erasure |
| a leaked pipeline                | all its threads, pools (MBs of frames), device fds, forever | nothing ever frees them |
| `gst_element_set_state(NULL)`    | joins every streaming thread           | must wait for threads to stop |

## 5. C vs C++

| Concern                  | C                                              | C++                                                |
|--------------------------|------------------------------------------------|----------------------------------------------------|
| Releasing refs           | manual `unref` on every path, or `g_autoptr`   | `std::unique_ptr<T, Deleter>`, automatic          |
| Early return / error     | `goto cleanup` pattern or `g_autoptr`          | destructors run automatically, also on exceptions |
| Giving ownership away    | just stop using the pointer / `g_steal_pointer`| `.release()` / `std::move`                         |
| Flag enums               | `A \| B` works                                 | `static_cast<GstXxxFlags>(A \| B)`                |
| C callbacks              | function pointer + `user_data`                 | captureless lambda + `user_data` → `std::function` |
| Shared state from callbacks | `g_atomic_int_*`, `GMutex`                  | `std::atomic`, `std::mutex`                        |
| Official bindings        | —                                              | there is no official C++ binding (gstreamermm is unmaintained); small RAII helpers like these are what most C++ projects use |

## 6. Where it is used in the real world

- **Long-running embedded apps** (a camera/AI app on a BeagleBone AI-64 that
  must run for months): one leaked buffer per frame at 30 fps is 2.6 M leaks a
  day; one leaked pipeline per reconnect keeps a TI codec instance or a V4L2
  device busy until the board is rebooted.
- **TI Edge AI apps** (TI's `edgeai-gst-apps` in C++) wrap pipelines, appsrc/
  appsink and buffers in C++ classes exactly like this.
- **Qt multimedia, WebKit, Chromium-on-Linux media code** use GLib smart-pointer
  wrappers (`GRefPtr` in WebKit) around GStreamer objects.
- GStreamer's own source uses `g_autoptr` everywhere in newer code.

## 7. Common mistakes and pitfalls

- **Unreffing after `gst_bin_add`** → you destroyed the bin's ref → crash later.
- **Forgetting the pad from `gst_element_get_static_pad`** → leaks the pad
  and keeps its element alive.
- **Calling `gst_element_get_bus()` in a loop** without unref → leaks a bus
  ref per iteration.
- **Unreffing a borrowed pointer** (`gst_sample_get_buffer`, `GST_MESSAGE_SRC`,
  `gst_caps_get_structure`) → double free.
- **Unreffing after `gst_pad_push` / `gst_app_src_push_buffer`** → those took
  your ref; you now unref a buffer that may already be gone.
- **Last unref without `set_state(NULL)`** → "Trying to dispose element ...,
  but it is in PLAYING instead of the NULL state" critical warning; threads and
  devices are not released properly.
- **Trusting a non-NULL `gst_parse_launch` result** — with a missing element it
  can return a partial pipeline *and* set the GError. Check both.
- **Wrapping in `shared_ptr` "to be safe"** → two refcounts that disagree.
- **Capturing lambdas passed as C callbacks** — does not compile; and a
  `std::function` stored in a local that dies before the pipeline stops →
  use-after-free in the streaming thread.
- **C++: forgetting `static_cast` on flag ORs** — compile error; casting to
  the *wrong* enum type compiles but is a silent bug.

How to find leaks: `GST_TRACERS=leaks GST_DEBUG=GST_TRACER:7 ./app` reports
every GstObject/GstMiniObject alive at `gst_deinit()` (call `gst_deinit()` at
the end of `main` for this to work). Lesson 12 covers tracers in depth.

## 8. Files in this lesson

| File | What it shows | Run |
|------|---------------|-----|
| [01_unique_ptr_deleters.cpp](01_unique_ptr_deleters.cpp) | deleters for GstElement/GstBus/GstPad/GstMessage/GstCaps/GError/gchar, NULL-before-unref deleter, `static_cast` on message flags | `make run P=gstreamer/09-cpp-raii-wrapper/01_unique_ptr_deleters.cpp` |
| [02_refcount_mistakes.cpp](02_refcount_mistakes.cpp) | prints refcounts: floating → sunk, +1 getters, caps property, a real leak detected with a weak pointer, then fixed | `make run P=gstreamer/09-cpp-raii-wrapper/02_refcount_mistakes.cpp` |
| [03_pipeline_class.cpp](03_pipeline_class.cpp) | move-only RAII `Pipeline`, per-message lambdas, appsink → `std::function`, constructor throws on parse error | `make run P=gstreamer/09-cpp-raii-wrapper/03_pipeline_class.cpp` |
| [04_g_autoptr.c](04_g_autoptr.c) | the C way: `g_autoptr`, `g_autofree`, `g_steal_pointer` | `make run P=gstreamer/09-cpp-raii-wrapper/04_g_autoptr.c` |

All four were checked with the leaks tracer: no GStreamer object is alive at
`gst_deinit()`. `--display` (on the built binary) shows the video for 01, 03, 04.

## 9. Exercises

1. *(easy)* In 02, call `gst_element_get_bus()` three times and unref only
   twice; run with `GST_TRACERS=leaks GST_DEBUG=GST_TRACER:7` and find the
   `GstBus` in the report. Then fix it.
2. *(easy)* Add a `BufferPtr` and `SamplePtr` alias to 01 and use them to
   pull one sample from an `appsink`.
3. *(medium)* Add `Pipeline::seek(double rate, GstClockTime pos)` to 03. You
   will need `static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
   GST_SEEK_FLAG_ACCURATE)`.
4. *(medium)* Write a generic `template <class T> struct GObjDeleter` that
   works for any `GObject` via `g_object_unref`, and a `make_element(factory,
   name)` that returns an `ElementPtr` holding a *sunk* (non-floating) ref.
   Then write `Bin::add(ElementPtr)` that transfers ownership correctly.
   Hint: after `ref_sink` you own a real ref, and `gst_bin_add` adds its own.
5. *(hard)* Make 03's `run()` use a `GMainLoop` and `gst_bus_add_watch`
   instead of polling, with a `GSource` RAII wrapper that removes the watch in
   its destructor.
6. *(hard)* Break 03 on purpose: store the `std::function` by value in a local
   instead of the map, and run under ASan. Explain the report.

## 10. Remember forever

```
REFS: get/new/make/pop/pull/request = YOURS (unref). sample_get_buffer/caps_get_structure/MESSAGE_SRC = BORROWED.
      push/send/post = TAKES yours. factory_make = FLOATING; gst_bin_add sinks it.
      g_object_set(caps) takes its own ref: still unref yours.
ORDER: set_state(NULL) -> gst_object_unref(pipeline). Always. Then gst_deinit().
C++:  unique_ptr<T, struct Deleter{ op()(T*) { xxx_unref } }>  (zero overhead)
      wrap on the line you receive it; .get() to lend; .release() to give away.
      flags: static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR)
      C callback = captureless lambda + user_data -> your object.
C:    g_autoptr(GstElement) x = ...; g_autofree gchar *s; return g_steal_pointer(&x);
FIND: GST_TRACERS=leaks GST_DEBUG=GST_TRACER:7 ./app
```

Review this card with the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

## 11. Self-test questions

1. What is a floating reference, which functions return one, and what does
   `gst_bin_add` do with it?
2. Which of these return a reference you must unref: `gst_bin_get_by_name`,
   `gst_sample_get_buffer`, `gst_element_get_static_pad`, `GST_MESSAGE_SRC`,
   `gst_pad_get_current_caps`, `gst_caps_get_structure`?
3. After `g_object_set(capsfilter, "caps", caps, NULL)`, who owns `caps`?
4. Why must the pipeline go to NULL before the last unref? Where does the RAII
   class guarantee that?
5. Why does `GST_MESSAGE_EOS | GST_MESSAGE_ERROR` compile in C but not in C++?
6. Why is the deleter a struct and not a function pointer?
7. Why `unique_ptr` and not `shared_ptr` for GObjects?
8. How would you prove that your program does not leak a single GstObject?
