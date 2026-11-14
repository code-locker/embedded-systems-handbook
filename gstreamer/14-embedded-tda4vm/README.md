# 14 — Embedded GStreamer on the TDA4VM (BeagleBone AI-64)

> On a PC GStreamer is a software framework. On the TDA4VM it schedules
> hardware: ISP, scaler, codec, DSP and display do the pixel work, and the
> A72 cores only decide where each frame goes next.

## 1. Why this exists

The BeagleBone AI-64 has a TI **TDA4VM** (J721E family) SoC: two Cortex-A72
cores, which on their own cannot colour-convert, scale, encode and run a
neural network on 1080p30 video. The rest of the chip can:

| Block | What it does for video | How GStreamer reaches it |
|-------|------------------------|--------------------------|
| **CSI-2 Rx** | camera interface | `v4l2src` (V4L2 capture device) |
| **VPAC: VISS** (ISP) | raw Bayer → YUV, 3A statistics | TI `tiovxisp` (TI SDK) |
| **VPAC: MSC** (multiscaler) | 1 input → several scaled outputs | TI `tiovxmultiscaler` (TI SDK) |
| **VPAC: LDC** | lens distortion correction | TI `tiovxldc` (TI SDK, check availability) |
| **C7x DSP + MMA** | deep-learning inference (TIDL) | TI `tidlinferer` (+ `tidlpreproc`/`tidlpostproc`) |
| **Video codec** | H.264/H.265 encode/decode | `v4l2h264enc` / `v4l2h264dec` (V4L2 M2M) |
| **GPU** (PowerVR) | GL composition/rendering | `glimagesink`, GL elements (driver dependent) |
| **DSS** (display) | scan-out planes | `kmssink`, `waylandsink` |

About the codec: TI documents the TDA4VM/J721E codec as Imagination
**D5520** (decoder) + **VXE384** (encoder) cores, driven by TI's V4L2 drivers
in its SDK kernel. Newer TI chips (J721S2, J784S4, AM62A) use the
Chips&Media **Wave5** VPU, which has a mainline Linux driver. Either way
userspace sees **V4L2 memory-to-memory devices**, and GStreamer's
`video4linux2` plugin (plugins-good) wraps them as `v4l2h264enc` /
`v4l2h264dec`. Check your SoC/SDK documentation for what is enabled in your image.

This lesson teaches how a pipeline maps onto that hardware, how to capture from
cameras, how to write one program that runs on the PC and the board, and
how to cross-compile it.

## 2. Mental model

**Analogy: an airport baggage system.** The A72 is the control tower, not
the conveyor belt. Bags (frames) sit in shared bins (**dmabufs**) that every
handling station (ISP, scaler, encoder, display) can reach directly. The
tower only says "bin #3 → scaler now". If any station insists on unpacking the
bag by hand (a CPU element such as `videoconvert`), the whole line slows to
the speed of one person with a trolley.

```
                     DDR (CMA region: physically contiguous, dmabuf-exported)
  ┌────────────────────────────────────────────────────────────────────────────┐
  │   [frame 0]   [frame 1]   [frame 2]   [frame 3]      <- buffer pool         │
  └────▲─────────────▲────────────▲──────────────▲─────────────────────────────┘
       │ DMA         │ DMA        │ DMA          │ DMA          CPU: 0 pixel copies
  CSI-2 Rx ──► VPAC VISS ──► VPAC MSC ─┬──► codec (H.264) ──► RTP/UDP (lesson 11)
  (v4l2src)   (tiovxisp)  (tiovxmultiscaler)
                                        ├──► colour conv ─► C7x/MMA TIDL ─► results
                                        │   (tiovxdlcolorconvert) (tidlinferer)
                                        └──► DSS display (kmssink)
  GStreamer on the A72: pads, caps negotiation, events, buffer refcounts, threads
```

## 3. How it works

### 3.1 One binary, PC and board (program `01`)

Never hard-code a hardware element into a program you also want to test on
the PC. Ask the registry:

```c
GstElementFactory *f = gst_element_factory_find("v4l2h264enc");  /* NULL if absent */
if (f) { gst_object_unref(f); use("v4l2h264enc"); } else use("x264enc"/"jpegenc");
```

The **rank** of a factory (`gst_plugin_feature_get_rank`) is how autopluggers
(`decodebin`, `playbin`, `encodebin`) choose: PRIMARY (256) beats SECONDARY
(128) beats MARGINAL (64); NONE (0) is never autoplugged. On TI images the
hardware decoders usually have a high rank so `decodebin` picks them
automatically; if a software decoder wins, raise the rank with
`GST_PLUGIN_FEATURE_RANK=v4l2h264dec:MAX` (environment variable).

Program `01` lists the registry's video encoders by rank, then chooses
`v4l2h264enc > x264enc > openh264enc > jpegenc` and runs. On this PC it uses
`jpegenc`; on a board image with the codec driver it uses the hardware.

### 3.2 Camera capture with v4l2src (program `02`, script `01`)

```
$ v4l2-ctl --list-devices                 # which /dev/videoN is the camera
$ v4l2-ctl -d /dev/video2 --list-formats-ext
        [0]: 'YUYV' (YUYV 4:2:2)   Size: 640x480  30 fps   Size: 1280x720 10 fps
        [1]: 'MJPG' (Motion-JPEG)  Size: 1280x720 30 fps   Size: 1920x1080 30 fps
```

- **USB UVC cameras** work on any image: `v4l2src ! video/x-raw,format=YUY2,...`
  or `image/jpeg,...` + a JPEG decoder for high resolutions.
- **CSI cameras** (e.g. IMX219, the Raspberry Pi v2 sensor) deliver **raw
  Bayer** (`video/x-bayer`). They need a device-tree overlay to enable the
  sensor and the TI ISP element `tiovxisp` with sensor tuning (DCC) files from
  the TI SDK image. Without the ISP you only get unprocessed Bayer data.
- `io-mode`: `auto`, `rw`, `mmap`, `userptr`, `dmabuf` (export the driver's
  buffers as dmabufs for zero-copy downstream), `dmabuf-import` (fill buffers
  that downstream allocated). Verified enum on this PC's `v4l2src`; M2M encoders
  have `output-io-mode` / `capture-io-mode`.
- A camera is a **live source**: it produces in real time, the pipeline
  does not preroll (`NO_PREROLL` from the state change), and slow consumers
  must drop, not queue: `queue leaky=downstream max-size-buffers=2`,
  `appsink drop=true max-buffers=2`.
- Program `02` uses exactly that downstream with `videotestsrc is-live=true`
  by default and `v4l2src` with `--camera /dev/videoN`. Note what it prints on
  the PC: the source's native caps are already **GRAY8** — negotiation pushed
  the consumer's format up to the source, so `videoconvert` was passthrough.
  A real camera cannot do GRAY8, so there `videoconvert` really works (on the CPU).

### 3.3 Hardware codecs through V4L2 M2M (script `02`)

```
            OUTPUT queue (raw frames in)          CAPTURE queue (bitstream out)
  upstream ──► [v4l2h264enc sink pad] ──VIDIOC_QBUF──► codec HW ──DQBUF──► [src pad] ──► h264parse
              output-io-mode=dmabuf-import          capture-io-mode=mmap/dmabuf
```

- Feed it its native format (typically **NV12**) so no CPU conversion is needed.
- Encoder settings are V4L2 controls passed via `extra-controls`, e.g.
  `extra-controls="controls,video_bitrate=4000000"`; list the driver's
  controls with `v4l2-ctl -d /dev/videoN -l`.
- With several M2M devices the plugin names elements `v4l2videoNh264enc`; use
  `gst-inspect-1.0 | grep v4l2` on the board.
- `h264parse` (gst-plugins-bad) is needed between encoder and muxer/payloader
  and before the decoder.

### 3.4 TI Edge AI GStreamer plugins (scripts `03`, `04`) — TI-specific

TI ships extra GStreamer plugins (`edgeai-gst-plugins`, built on TIOVX =
TI's OpenVX) **only in its SDK images** for J721E/TDA4VM (Edge AI SDK,
Processor SDK Linux). They are **not** in upstream GStreamer or plain Debian:

| Element | Hardware | Role |
|---------|----------|------|
| `tiovxisp` | VPAC VISS | raw sensor → NV12, needs sensor DCC files |
| `tiovxmultiscaler` | VPAC MSC | one input, several outputs at different sizes (request pads) |
| `tiovxdlcolorconvert` | TIOVX kernel | colour conversion (e.g. NV12 → RGB for a model) |
| `tiovxldc` | VPAC LDC | lens distortion correction |
| `tidlpreproc` | C7x/A72 | normalise/lay out the tensor the model expects |
| `tidlinferer` | C7x DSP + MMA | runs a TIDL-compiled model |
| `tidlpostproc` | A72 | draws detections/segmentation onto the image |
| `kmssink` | DSS | display (upstream plugins-bad, used on TI images) |

**Property names change between SDK releases**, so this course deliberately
does not list them: run `gst-inspect-1.0 <element>` on your board, and copy
working pipelines from TI's `edgeai-gst-apps`, which builds a complete
`gst-launch` string from a YAML config and prints it. Scripts `03` and `04`
check whether the elements exist and exit with a message on the PC.

### 3.5 Cross-compiling for aarch64 (script `05`)

Three options, simplest first:

1. **Build on the board.** `sudo apt install build-essential pkg-config
   libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev`, copy the repo, `make
   run P=...`. Slow, but zero configuration. Fine for course-sized programs.
2. **Cross-compile with a sysroot** copied from the board:

   ```
   rsync -a --relative board:/usr/include board:/usr/lib/aarch64-linux-gnu \
                       board:/lib/aarch64-linux-gnu ~/ai64-sysroot/
   export PKG_CONFIG_SYSROOT_DIR=~/ai64-sysroot
   export PKG_CONFIG_LIBDIR=~/ai64-sysroot/usr/lib/aarch64-linux-gnu/pkgconfig
   aarch64-linux-gnu-gcc --sysroot=~/ai64-sysroot app.c -o app \
       $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0)
   ```

   `PKG_CONFIG_LIBDIR` **replaces** the search path (so host x86 `.pc` files
   are never found); `PKG_CONFIG_SYSROOT_DIR` prefixes `-I`/`-L` with the sysroot.
3. **Debian multiarch**: `dpkg --add-architecture arm64`, install the
   `:arm64` dev packages, `PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig`.
4. For meson projects (e.g. a plugin from lesson 10), a **cross file**:

   ```ini
   [binaries]
   c = 'aarch64-linux-gnu-gcc'
   cpp = 'aarch64-linux-gnu-g++'
   pkg-config = 'pkg-config'
   [properties]
   sys_root = '/home/me/ai64-sysroot'
   pkg_config_libdir = '/home/me/ai64-sysroot/usr/lib/aarch64-linux-gnu/pkgconfig'
   [host_machine]
   system = 'linux'
   cpu_family = 'aarch64'
   cpu = 'cortex-a72'
   endian = 'little'
   ```

   `meson setup build-arm --cross-file ai64.ini && ninja -C build-arm`.

Match versions: build against the **board's** GStreamer headers (the TI SDK
image may ship a different GStreamer version than your PC). Deploy with
`scp app debian@<board>:~`, run over `ssh`. Without a display: use
`fakesink`/`appsink`, or `kmssink` straight to HDMI/DP with no desktop.

### 3.6 Bring-up checklist on the board

```
uname -a; cat /proc/device-tree/model                    # right kernel/board
gst-inspect-1.0 | grep -Ei 'v4l2|tiovx|tidl|kms'         # which HW elements exist
v4l2-ctl --list-devices                                  # capture + M2M devices
gst-launch-1.0 -v videotestsrc num-buffers=100 ! fakesink   # GStreamer itself works
gst-launch-1.0 -v v4l2src device=/dev/videoN num-buffers=100 ! fakesink  # camera
GST_DEBUG=2,v4l2*:5 ...                                  # when the camera fails
GST_TRACERS=rusage GST_DEBUG=GST_TRACER:7 ...            # who uses the CPU
cat /sys/kernel/debug/dma_buf/bufinfo                    # dmabufs alive (root, debugfs)
```

## 4. Complexity table

| Path for 1080p30 NV12 (3.1 MB/frame, 93 MB/s) | A72 CPU cost | Copies | Latency |
|---------|------------|--------|---------|
| v4l2src (dmabuf) → HW encoder (dmabuf-import) | ~a few % (scheduling) | 0 | ~1–2 frames |
| v4l2src → `videoconvert` → HW encoder | 1 core heavily loaded | 1 + per-pixel math | +ms per frame |
| v4l2src → `x264enc` (software) | several cores, may not keep up at 1080p30 | n/a | high |
| MSC scale 1080p → 3 outputs | ~0 | 0 (HW writes 3 outputs) | sub-frame |
| `videoscale` 1080p → 3 outputs on CPU | ~1 core | 3 | ms each |
| TIDL on C7x/MMA | ~0 on A72 | tensor in/out | model dependent |
| CPU map of a dmabuf (e.g. `appsink` reading pixels) | cache maintenance O(size) | 0–1 | can be slow on uncached memory |

Rule: count the elements that touch pixels on the CPU. Target: zero in the
steady-state video path.

## 5. C vs C++

Nothing here is language specific: element selection, caps and properties are
the same calls. On the board prefer:

- C for tiny tools and plugins (one less runtime library on minimal images).
- C++ (lesson 09's RAII wrapper) for applications with several pipelines,
  restart logic and error handling — RAII matters more on a device that runs
  for months, where a leaked buffer per hour is an out-of-memory crash per week.
- In C++, `v4l2` / hardware element names belong in config, not in code: read
  them from a file so one binary serves several boards.

## 6. Where it is used in the real world

- **TI Edge AI SDK demos** on the BeagleBone AI-64 and TDA4VM EVM: camera →
  ISP → multiscaler → TIDL object detection → display or RTSP, built from the
  plugins above.
- **Automotive ADAS / surround view** (the TDA4VM's target market): camera
  capture, LDC (fish-eye correction), stitching, recording with the codec.
- **Robotics** (ROS 2 with GStreamer camera nodes): `v4l2src` → hardware
  encoder → RTP to a ground station (lesson 11).
- **Industrial vision**: `v4l2src` → `appsink drop=true max-buffers=1` → your
  C code (lesson 06), always processing the newest frame.
- **Every Yocto-based camera product**: same structure, element names from
  that vendor's SDK.

## 7. Common mistakes and pitfalls

1. **Copy-pasting a TI pipeline from another SDK version.** Element
   properties change. `gst-inspect-1.0` on *your* image is the truth.
2. **Expecting TI elements on a plain Debian image.** They come only with
   TI's SDK images.
3. **Raw sensor with no ISP**: `v4l2src` from an IMX219 gives Bayer;
   `videoconvert` cannot debayer it properly (that is `bayer2rgb`,
   plugins-bad, on the CPU — slow). Use `tiovxisp`.
4. **A hidden `videoconvert` in a "hardware" pipeline** because caps did not
   match. Check with `gst-launch-1.0 -v` that it is passthrough, or remove it
   and see whether linking still works.
5. **Wrong `/dev/videoN`**: numbering changes with boot order and overlays;
   use `v4l2-ctl --list-devices` or the stable `/dev/v4l/by-id/` /
   `by-path/` links.
6. **Running the camera at a mode it does not list** → `not-negotiated`.
   Only request modes from `--list-formats-ext`.
7. **Cross-compiling against the PC's headers** (PKG_CONFIG_PATH instead of
   PKG_CONFIG_LIBDIR) → links on the PC, fails or misbehaves on the board.
8. **Unbounded queues on a live camera** → memory and latency grow until the
   OOM killer stops your demo.
9. **CMA too small** for the number of buffers × resolution → buffer
   allocation errors from v4l2. CMA size is set on the kernel command line
   or in the device tree.

## 8. Files in this lesson

| File | What it shows | Run |
|------|---------------|-----|
| [01_pick_best_element.c](01_pick_best_element.c) | registry lookup + rank listing, runtime choice HW > SW encoder, same binary on PC and board | `make run P=gstreamer/14-embedded-tda4vm/01_pick_best_element.c` |
| [02_camera_or_test.c](02_camera_or_test.c) | live capture path (leaky queue, dropping appsink), `--camera /dev/videoN` for v4l2src, prints negotiated and native caps | `make run P=gstreamer/14-embedded-tda4vm/02_camera_or_test.c` |
| [01_v4l2_camera_preview.sh](01_v4l2_camera_preview.sh) | `v4l2-ctl --list-formats-ext` + capture 100 frames (UVC); MJPEG variant | `bash gstreamer/14-embedded-tda4vm/01_v4l2_camera_preview.sh /dev/video0 [--display]` — needs a camera |
| [02_hw_h264_encode.sh](02_hw_h264_encode.sh) | `v4l2h264enc` encode to file, `v4l2h264dec` decode, camera→RTP sketch | board with codec driver + plugins-bad |
| [03_tiovx_scaler_colorconvert.sh](03_tiovx_scaler_colorconvert.sh) | MSC multiscaler to 2 sizes + `tiovxdlcolorconvert` to RGB; ISP sketch | TI SDK image only |
| [04_tidl_inference.sh](04_tidl_inference.sh) | shape of a TIDL pipeline (placeholders for SDK-specific properties) | TI SDK image only |
| [05_cross_compile.sh](05_cross_compile.sh) | cross-compile a course program for aarch64 with a sysroot or multiarch | `bash gstreamer/14-embedded-tda4vm/05_cross_compile.sh [file.c]` (from repo root) |

Verified on the course PC: both programs; the scripts' "element/camera
missing" paths. **Not verified here** (need the board, a camera, and/or TI's
SDK image): the actual camera capture, hardware codec, TIOVX and TIDL
pipelines, and an end-to-end cross build (no arm64 GStreamer dev files on the PC).

Build requirements (PC and board): `sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-tools`; for cameras `v4l-utils`; for cross builds `gcc-aarch64-linux-gnu`.

## 9. Exercises

1. *(easy)* Run `01` with `GST_PLUGIN_FEATURE_RANK=jpegenc:NONE` set. Does the
   choice change? Why not? *(Hint: `gst_element_factory_find` ignores rank;
   autopluggers don't.)* Change `01` to skip candidates with rank NONE.
2. *(easy, board)* Run the bring-up checklist (3.6) on your BeagleBone AI-64 and
   write down which hardware elements your image has.
3. *(medium)* Extend `02` to accept `--mjpeg`: request `image/jpeg` from the
   camera and add `jpegdec`. Compare CPU with `rusage` against YUY2.
4. *(medium)* Make `01` also pick a decoder (`v4l2h264dec` > `avdec_h264`)
   and build an encode→decode loop that checks the decoded frame count.
5. *(medium, board)* Cross-compile `02_camera_or_test.c` with `05_cross_compile.sh`,
   deploy with `scp`, run with `--camera`.
6. *(hard, board + TI SDK)* Replace `videoconvert` in `02` with
   `tiovxdlcolorconvert` when available (runtime selection like `01`), and
   measure the CPU difference with the rusage tracer.
7. *(hard, board + TI SDK)* Take a pipeline printed by TI's edgeai-gst-apps,
   dump its dot graph (lesson 12), and label each element with the hardware
   block that executes it.

## 10. Remember forever

```
TDA4VM: A72 = control, VPAC (VISS ISP, MSC scaler, LDC), C7x+MMA = TIDL, codec = V4L2 M2M, DSS = kmssink
v4l2h264enc/dec = V4L2 M2M (plugins-good); feed NV12; settings via extra-controls; h264parse
tiovx*/tidl* = TI SDK image ONLY; property names: gst-inspect-1.0 on the board, never from memory
camera: v4l2-ctl --list-formats-ext -> request a listed mode; Bayer needs ISP; live => leaky queues
zero copy = dmabuf end to end; any CPU element (videoconvert) in the chain breaks it
one binary: gst_element_factory_find() at runtime, HW first, SW fallback
cross: aarch64-linux-gnu-gcc --sysroot + PKG_CONFIG_LIBDIR (replace!) + PKG_CONFIG_SYSROOT_DIR
```

Review it with the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

## 11. Self-test questions

1. Name the TDA4VM hardware block behind each of: camera ISP, multi-output
   scaling, DL inference, H.264 encode, display. Which GStreamer element reaches each?
2. Why must the TI plugin property names be checked on the board rather than
   copied from a tutorial?
3. Why does a `videoconvert` between `v4l2src` and `v4l2h264enc` hurt more on the
   TDA4VM than the same element on a desktop?
4. What does `io-mode=dmabuf` on `v4l2src` change, and what must the next
   element support to benefit?
5. Why does a CSI IMX219 camera need `tiovxisp` while a USB webcam does not?
6. How does program `01` keep one binary working on both the PC and the board?
7. What is the difference between `PKG_CONFIG_PATH` and `PKG_CONFIG_LIBDIR`, and
   which one do you need for cross-compiling? Why?
8. Live camera, slow consumer: which two properties keep latency bounded?
