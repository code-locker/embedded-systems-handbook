# 01 — gst-launch-1.0: build pipelines from the shell

> Prototype every pipeline on the command line first. If it does not work in
> `gst-launch-1.0`, it will not work in C either, and the shell is 100× faster
> to iterate.

## 1. Why this exists

Before you write C you need to know *which* elements, *which* caps and
*which* properties make the pipeline work. `gst-launch-1.0` takes a textual
pipeline description, builds it, sets it to PLAYING, prints bus messages and
exits on EOS or error. Along with `gst-inspect-1.0` (what does this element
do, which properties, which caps?) and `gst-discoverer-1.0` (what is inside
this file/stream?) it is the GStreamer engineer's daily toolkit, on the PC
and over SSH on the BeagleBone AI-64.

The *same* syntax is accepted by `gst_parse_launch()` in C, so the string you
debugged in the shell can be pasted into your program (lesson 02).

## 2. Mental model

**Analogy: a shell pipe for media.** `cat file | grep x | sort` connects
programs with `|`; `filesrc ! decodebin ! autovideosink` connects elements
with `!`. The difference: GStreamer "pipes" are typed (caps) and the
elements run in threads inside **one** process.

```
  gst-launch-1.0  videotestsrc num-buffers=100 ! video/x-raw,width=320 ! fakesink sync=false
                  └─element─┘ └──property───┘   └────caps filter────┘   └element┘ └property┘
                                             ▲                       ▲
                                             └──── "!" = link ───────┘
```

## 3. How it works

### 3.1 The grammar, in 8 rules

| Syntax | Meaning | Example |
|--------|---------|---------|
| `a ! b` | link a's src pad to b's sink pad (any compatible pads) | `videotestsrc ! fakesink` |
| `elem prop=value` | set a property | `videotestsrc pattern=ball num-buffers=60` |
| `type/sub,field=val,...` | insert a `capsfilter` with these caps | `! video/x-raw,format=NV12,width=640 !` |
| `name=x` then `x.` | name an element, refer to it later (request/sometimes pad) | `tee name=t  t. ! queue ! ...` |
| `x.padname` | link a specific pad | `demux.video_0 ! queue ! ...`, `mux.audio_%u` |
| `elem pad::prop=v` | set a property on a (request) pad | `compositor sink_1::xpos=10` |
| `( ... )` | a bin | rarely needed |
| spaces separate chains | several chains in one pipeline | `mux. ! filesink   src1 ! mux.  src2 ! mux.` |

Field types in caps can be explicit: `width=(int)640`,
`framerate=(fraction)30/1`, `format=(string){NV12,I420}` (a list),
`width=[ 1, 1920 ]` (a range). Quote caps with shell metacharacters:
`'video/x-raw,format=(string)NV12'`.

### 3.2 The useful flags

```
 -v   verbose: print every property change, especially "caps = ..." of every
      pad once negotiated → THE way to see what format each link uses.
 -e   on Ctrl-C send EOS first, wait for it to reach the sinks, then stop.
      Without -e, muxers (mp4mux/matroskamux) never write their index and
      the file is broken. Always use -e when recording.
 -m   print every bus message (state changes, stream-start, latency, qos...).
 -t   print tags (title, codec, bitrate) found in the stream.
 -q   quiet.
 --gst-debug=3 / GST_DEBUG=3   debug log (lesson 12).
```

### 3.3 What gst-launch does internally

```
 gst_parse_launch(argv) ─► pipeline (bin)
     │
 set_state(PAUSED) ──► preroll: data flows until every sink holds 1 buffer
     │                 (live sources skip this: NO_PREROLL)
 set_state(PLAYING) ─► clock starts, sinks render on time
     │
 loop on the bus:  ERROR → print + stop
                   EOS   → "Got EOS from element..." → stop
                   Ctrl-C → (with -e: send EOS, wait) → stop
     │
 set_state(NULL), unref
```

Exactly the skeleton you will write in C in lesson 02.

### 3.4 Delayed linking: sometimes pads

`filesrc ! matroskademux ! jpegdec` works even though `matroskademux` has no
src pad yet when the pipeline is built: gst-launch connects to the
`pad-added` signal and links when the pad appears. In C you do this yourself
(lesson 04). With `demux.video_0` you choose which stream.

### 3.5 Tee and named elements

```
 videotestsrc ! tee name=t   t. ! queue ! sinkA   t. ! queue ! sinkB

                      ┌─ t.src_0 ─► queue ─► sinkA
 videotestsrc ─► tee ─┤
                      └─ t.src_1 ─► queue ─► sinkB
```

Each `t.` requests a new pad from the tee. Each branch needs a `queue`
(lesson 08 explains the preroll deadlock you get without it).

### 3.6 The companion tools

```
 gst-inspect-1.0                 list all elements
 gst-inspect-1.0 jpegenc         pads, caps, properties (with ranges/defaults)
 gst-inspect-1.0 --exists x264enc && echo yes   scriptable presence check
 gst-inspect-1.0 -b              blacklisted plugins (failed to load!)
 gst-discoverer-1.0 file.mkv     streams, codecs, duration, resolution
 gst-play-1.0 file.mkv           simple player with keyboard seek
 gst-typefind-1.0 file           detected media type
```

## 4. Complexity table

| Pipeline piece | Cost per frame | Why |
|----------------|----------------|-----|
| `videotestsrc` | O(pixels) | draws each frame in software |
| caps filter    | ~0 | only restricts negotiation, never touches data |
| `videoconvert` | 0 if passthrough, else O(pixels) | it does nothing when in/out caps are equal |
| `videoscale`   | O(output pixels × taps) | resampling filter |
| `videorate`    | ~0 | drops/duplicates buffer *references* |
| `jpegenc`      | O(pixels), heavy | DCT + entropy coding on the CPU |
| `tee`          | O(branches) refs | buffers are shared, never copied |
| `queue`        | 1 lock + thread switch | adds latency equal to its fill level |
| `fakesink sync=false` | ~0 | measures the rest of the pipeline at full speed |

## 5. C vs C++

There is no C/C++ difference on the command line. The bridge to code is
`gst_parse_launch("videotestsrc ! ... ! fakesink", &err)` (C and C++ alike),
used in lesson 02. Long-term, building elements one by one with
`gst_element_factory_make` + `gst_element_link` gives you compile-time
names, error checks per link, and direct pointers to the elements.

## 6. Where it is used in the real world

* Board bring-up: "does the camera work?" = `gst-launch-1.0 v4l2src ! fakesink -v`.
* TI's Edge AI SDK documentation is mostly gst-launch pipelines with
  `tiovx*` / `tidl*` elements: you read them like the ones here (lesson 14).
* systemd services on products sometimes literally run a gst-launch line
  (quick, but no error recovery: use a real app in production).
* Bug reports to GStreamer always include a gst-launch reproduction.

## 7. Common mistakes and pitfalls

1. **Recording without `-e`** → unplayable MP4/MKV after Ctrl-C.
2. **No `num-buffers`** with test sources → never ends (fine interactively,
   not in scripts/CI).
3. **Missing `queue` after `tee`** → hangs in PAUSED (no error, just stuck).
4. **Unquoted caps with parentheses** → bash syntax error. Quote them.
5. **`videoconvert` everywhere "just in case"** → fine on a PC, CPU-expensive
   on the board when it really converts. Use `-v` to see whether it does.
6. **`WARNING: erroneous pipeline: no element "x264enc"`** → plugin not
   installed (plugins-ugly) or blacklisted (`gst-inspect-1.0 -b`).
7. **`could not link a to b`** → no common caps. Read both elements' pad
   caps with `gst-inspect-1.0`, insert a converter.
8. **Assuming every sink behaves like `fakesink`**: `fakesink` defaults to
   `sync=false` (as fast as possible), but real sinks (`autovideosink`,
   `alsasink`, `appsink`) default to `sync=true` (real time). The scripts
   write `sync=false` explicitly so the intent stays visible when you swap
   the sink; add `sync=true` to `fakesink` to emulate a real display.
9. Passing a bin description through a shell variable: write `$SINK`
   unquoted if it contains properties (`SINK="fakesink sync=false"`), because
   gst-launch treats each argument as one token.

## 8. Files in this lesson

All scripts terminate on their own (num-buffers) and default to `fakesink`.
Most accept `SINK=autovideosink` to show a window.

| File | What it shows | Run |
|------|---------------|-----|
| [01_hello_test_pattern.sh](01_hello_test_pattern.sh) | source ! sink | `bash gstreamer/01-gst-launch/01_hello_test_pattern.sh` |
| [02_caps_filter.sh](02_caps_filter.sh) | caps filter, `-v` to see negotiated caps | `bash gstreamer/01-gst-launch/02_caps_filter.sh` |
| [03_scale_and_rate.sh](03_scale_and_rate.sh) | `videoscale` + `videorate` driven by caps | `bash gstreamer/01-gst-launch/03_scale_and_rate.sh` |
| [04_audio_tone.sh](04_audio_tone.sh) | `audiotestsrc`, `audioconvert`, `audioresample` | `bash gstreamer/01-gst-launch/04_audio_tone.sh` |
| [05_encode_to_file.sh](05_encode_to_file.sh) | `jpegenc ! matroskamux ! filesink`, `-e` | `bash gstreamer/01-gst-launch/05_encode_to_file.sh` |
| [06_decode_file.sh](06_decode_file.sh) | manual demux+decode vs `decodebin` | `bash gstreamer/01-gst-launch/06_decode_file.sh` |
| [07_playbin.sh](07_playbin.sh) | `playbin` with custom sinks | `bash gstreamer/01-gst-launch/07_playbin.sh` |
| [08_tee_two_branches.sh](08_tee_two_branches.sh) | `tee` + `queue`: display and record | `bash gstreamer/01-gst-launch/08_tee_two_branches.sh` |
| [09_audio_video_mux.sh](09_audio_video_mux.sh) | two sources into one muxer, `gst-discoverer-1.0` | `bash gstreamer/01-gst-launch/09_audio_video_mux.sh` |
| [10_overlay_and_compositor.sh](10_overlay_and_compositor.sh) | `timeoverlay`, `compositor` picture-in-picture, pad properties | `bash gstreamer/01-gst-launch/10_overlay_and_compositor.sh` |
| [11_rtp_udp_loopback.sh](11_rtp_udp_loopback.sh) | RTP/JPEG over UDP to localhost, caps on `udpsrc` | `bash gstreamer/01-gst-launch/11_rtp_udp_loopback.sh` |
| [12_camera_and_hw_board.sh](12_camera_and_hw_board.sh) | **board only**: `v4l2src`, `v4l2h264enc` (+ `h264parse` from plugins-bad); skips cleanly on a PC | `bash gstreamer/01-gst-launch/12_camera_and_hw_board.sh` |

Elements used: core + plugins-base + plugins-good only (scripts 01–11).
Install: `sudo apt install gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good`.

## 9. Exercises

1. *(easy)* Run `02_caps_filter.sh` and find, in the `-v` output, the caps on
   `videoconvert0.src`. Change the format to `NV12`; what changes?
2. *(easy)* Add `-m` to `01_hello_test_pattern.sh`. List the message types
   you see, in order.
3. *(medium)* Make `03_scale_and_rate.sh` output 15 fps 640x360. Predict the
   number of frames that reach the sink, then verify with
   `fakesink silent=false -v | grep -c chain`.
4. *(medium)* Break `08_tee_two_branches.sh` by removing both `queue`s. What
   happens? (Hint: Ctrl-C after a few seconds, then read lesson 08.)
5. *(medium)* Record 5 s of audio + video into Ogg (`oggmux`, `theoraenc`,
   `vorbisenc`) and check it with `gst-discoverer-1.0`.
6. *(hard)* Write a pipeline that shows four `videotestsrc` patterns in a 2×2
   grid with `compositor` (pad properties `xpos`, `ypos`).
7. *(hard, board)* On the BeagleBone AI-64 run
   `gst-inspect-1.0 | grep -i -E 'v4l2|tiovx|tidl'` and write down which
   hardware elements your image provides.

## 10. Remember forever

```
gst-launch-1.0 [-v -e -m] elem prop=val ! type/sub,field=val ! elem
"!" links; caps between "!" = capsfilter; name=x then x. = that element.
-v shows negotiated caps; -e makes Ctrl-C send EOS (valid files).
Tests: num-buffers=N + fakesink sync=false → terminates fast.
tee → queue on EVERY branch.  Demux pads link late (sometimes pads).
gst-inspect-1.0 X: pads/caps/properties.  gst-discoverer-1.0 file: contents.
```

Review this card on the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

## 11. Self-test questions

1. What does `video/x-raw,width=320` between two `!` turn into?
2. Why does a recording made without `-e` often not play?
3. How do you link to the *second* branch of a tee? To the video stream of a demuxer?
4. What does `sync=false` change on `fakesink`, and when is it wrong to use it?
5. Which tool tells you an element's properties? Which tells you what is in a file?
6. Why can `filesrc ! matroskademux ! jpegdec` be linked although the demuxer has no src pad at parse time?
7. Why does a UDP receiver need explicit caps?
8. What is the C function that accepts the same syntax as gst-launch?
