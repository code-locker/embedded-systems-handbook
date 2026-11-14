# 11 — Network streaming: RTP, UDP, TCP, RTSP

> Prerequisites: lessons 05 (caps), 06 (appsink), 08 (queues/threads).
> Dev headers: `sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev`

## 1. Why this exists

A BeagleBone AI-64 with a camera is rarely looked at directly: the video goes
over Ethernet to a laptop, a recorder, a cloud service, or another board.
Doing that well means answering four questions:

1. **How do I cut a video stream into network packets** and glue it back
   together? → *payloaders* / *depayloaders* (RTP).
2. **What happens when packets arrive late, out of order, or not at all?**
   → *jitterbuffer*, keyframes, UDP vs TCP.
3. **How does the receiver know what the stream is** (codec, size, clock)?
   → caps out-of-band: SDP, RTSP.
4. **How low can latency go**, and what knobs move it?

GStreamer has a brick for each answer; this lesson wires them together and
measures them, using nothing but `127.0.0.1`.

## 2. Mental model

**Streaming is shipping furniture flat-packed by post.**

```
 SENDER                                                  RECEIVER
 ┌────────┐ ┌────────┐ ┌──────────┐            ┌──────────┐ ┌──────────┐ ┌────────┐
 │ build  │►│ shrink │►│ flat-pack│►─ post ───►│ sorting  │►│ assemble │►│ unwrap │►
 │ (src)  │ │ (enc)  │ │ (pay)    │  (UDP)     │ table    │ │ (depay)  │ │ (dec)  │
 └────────┘ └────────┘ └──────────┘            │ (jitter- │ └──────────┘ └────────┘
                         numbered boxes        │  buffer) │
                         ≤ 1400 B each         └──────────┘
                         + label: seq, time    waits a little for late boxes,
                                               puts them back in order
```

* The **payloader** is the flat-packer: it cuts each frame into boxes that fit
  through the letterbox (the **MTU**, ~1500 bytes on Ethernet) and writes a
  label on each: sequence number, timestamp, payload type.
* The postal service (**UDP**) is fast but may lose, duplicate or reorder
  boxes. It does not care about your furniture.
* The **jitterbuffer** is a sorting table: it holds boxes for `latency` ms so
  late ones can catch up, then hands them over in order.
* The **depayloader** re-assembles the frame; if one box is missing, the
  whole frame (or a slice of it) is lost.
* The assembly instructions — "it's a 640x480 JPEG at 90 kHz" — are **not in
  the boxes**. They travel separately: that's the **SDP** (or the `caps=` you
  type on `udpsrc`).

### One RTP packet

```
  ┌──────────── IP header (20 B) ─────────────┐
  ├──────────── UDP header (8 B): ports ──────┤
  ├──────────── RTP header (12 B) ────────────┤
  │ V=2 │P│X│CC│M│  PT (7 bits) │ sequence nr (16 bits) │
  │ M = marker: "last packet of this frame"    │
  │            timestamp (32 bits, 90 kHz for video)       │
  │            SSRC (32 bits, random stream id)            │
  ├──────────── payload header (codec specific) ┤  e.g. JPEG: type, Q, w/8, h/8
  │            payload: a slice of the frame    │  ≤ MTU - 40 - hdrs
  └────────────────────────────────────────────┘
```

* **Sequence number** → detect loss and reorder.
* **Timestamp** (90 kHz clock for all video) → when to show it; all packets of
  one frame share it.
* **Payload type** → which codec; 0–95 static (26 = JPEG), 96–127 dynamic
  (meaning given by the SDP: "96 = H264").
* **RTCP** (a sibling protocol, usually port+1) carries sender reports
  (map RTP timestamps to wall-clock NTP time → lip-sync, multi-camera sync)
  and receiver reports (loss, jitter). `rtpbin` handles RTP+RTCP sessions; the
  programs here use plain RTP for clarity.

## 3. How it works

### 3.1 The two pipelines of program 01

```
 SENDER thread (videotestsrc streaming thread)
 videotestsrc is-live ─► I420 320x240@30 ─► jpegenc ─► rtpjpegpay mtu=1400 ─► udpsink
                                             ~10 KB      ~8 packets/frame     sendto()
                                                                                │
 ════════════════════════════ kernel: 127.0.0.1, UDP socket buffer ═════════════╪════
                                                                                ▼
 RECEIVER thread 1 (udpsrc)          RECEIVER thread 2 (jitterbuffer's own task)
 udpsrc port=0 caps=application/x-rtp ─► rtpjitterbuffer latency=100
                                         ║ (pushes in order after ≤100 ms)
                                         ╚═► rtpjpegdepay ─► jpegdec ─► appsink
```

Things program 01 proves (and you should remember):

* **Start the receiver first.** UDP has no connection; packets sent to a port
  nobody has bound are dropped by the kernel.
* `udpsrc port=0` lets the kernel pick a free port; after the state change
  to READY the `port` property holds the real one. That's how the program
  runs in parallel with itself without collisions.
* **EOS does not cross the network.** The sender's EOS stops at `udpsink`.
  The receiver decides "the stream is over" by a timeout (here: 1 s with no
  frame). Real receivers use `udpsrc timeout=` (posts a message) or RTCP BYE.
* `rtpjpegpay` needs **4:2:0 or 4:2:2** JPEG (RFC 2435). Without
  `format=I420` videotestsrc negotiated a 4:4:4 format and the payloader
  printed `Invalid component` and sent nothing — 0 frames received. Caps
  matter even on the sender.

### 3.2 Caps travel out of band (SDP) — program 02

Raw video over RTP (RFC 4175, `rtpvrawpay`) shows the problem at its
clearest: the receiver must know sampling, depth, width, height,
colorimetry — none of it is inside the packets. Program 02 reads the
payloader's negotiated caps and gives them to `udpsrc`; this is exactly
what an **SDP** description does:

```
 payloader src caps                          SDP lines
 ────────────────────────────────            ──────────────────────────────
 application/x-rtp, media=video,         →   m=video 5000 RTP/AVP 96
   payload=96, clock-rate=90000,         →   a=rtpmap:96 raw/90000
   encoding-name=RAW,
   sampling=YCbCr-4:2:0, depth=8,        →   a=fmtp:96 sampling=YCbCr-4:2:0;
   width=64, height=48,                         depth=8; width=64; height=48;
   colorimetry=BT601-5                          colorimetry=BT601-5
```

RTSP's `DESCRIBE` returns that SDP, so `rtspsrc` never needs a hand-written
caps string. For a bare `udpsrc` you write it yourself.

### 3.3 The jitterbuffer, frame by frame

```
 arrival:   seq 10 ─ 11 ─ 13 ─────── 12 ─ 14 ─ 16 ──────────────── (15 lost)
 time  ms:   0      2     4          30   33   36
                            │
 jitterbuffer latency=50 ms │ holds each packet until (its RTP time + 50 ms)
                            ▼
 output:    10 11 12 13 14 ... waits for 15 until its deadline ... gives up,
            pushes 16 and a "packet-lost" event (depayloader drops that frame)
```

* `latency` (ms): bigger → survives more jitter/reordering, adds exactly that
  much delay. LAN: 20–50 ms. Wi-Fi/Internet: 200+ ms.
* `drop-on-latency=true`: never hold more than `latency` worth (live view).
* `mode`: how RTP timestamps are mapped to the local clock (`slave` default,
  `synced`, `none`). Default is fine unless you sync multiple senders.
* `do-retransmission`: request lost packets via RTCP NACK (needs rtpbin +
  RTX on the sender) — the middle ground between UDP and TCP.

### 3.4 UDP vs TCP

| | RTP over UDP | TCP (`tcpserversink`, HTTP MJPEG, RTSP-interleaved) |
|---|---|---|
| Loss | possible; codec/RTP must cope | never; retransmitted |
| Latency under loss | constant (frame dropped) | grows (head-of-line blocking) |
| Multicast (1→N cheaply) | yes | no |
| Firewalls/NAT | painful | easy |
| Framing | RTP packets | you need it: `multipartmux`, `matroskamux`, `gdppay` |
| Typical use | cameras on a LAN, video calls, robotics | recording over WAN, browsers, "it must not glitch" |

`06_tcp_mjpeg_loopback.sh` shows TCP: the first frames are missed until the
client connects, but nothing is lost after that.

### 3.5 H.264: what real products send

```
 v4l2src/videotestsrc ─► ENCODER ─► h264parse ─► rtph264pay ─► udpsink
                          │            │config-interval=-1: repeat SPS/PPS
                          │            │ before every IDR so late joiners decode
                          │ desktop: x264enc tune=zerolatency  (plugins-ugly)
                          │ TDA4VM : v4l2h264enc (TI hardware encoder, TI SDK image)
                          ▼
            IDR every key-int frames: recovery point after loss/late join
```

Elements and packages: `x264enc` → gstreamer1.0-plugins-ugly; `h264parse`
→ gstreamer1.0-plugins-bad; `avdec_h264` → gstreamer1.0-libav;
`rtph264pay/depay` → plugins-good. On a stock desktop with only
base+good, VP8 (`vp8enc` / `rtpvp8pay`, plugins-good) is a fully working
substitute: see `03_rtp_vp8_loopback.sh`.

### 3.6 RTSP server (gst-rtsp-server)

RTSP is a *control* protocol (like HTTP: DESCRIBE, SETUP, PLAY, TEARDOWN);
the media still flows as RTP (UDP, or interleaved in the TCP connection).
`gst-rtsp-server` is a library (not an element) that turns a launch line
into an RTSP endpoint. **It is not installed here**: it needs
`sudo apt install libgstrtspserver-1.0-dev gstreamer1.0-rtsp`, so this code is
a sketch and is not built by the Makefile:

```c
/* rtsp_server.c — NOT BUILT HERE. Needs gstreamer-rtsp-server-1.0:
 *   gcc rtsp_server.c -o rtsp_server \
 *       $(pkg-config --cflags --libs gstreamer-rtsp-server-1.0)          */
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    GstRTSPServer *server = gst_rtsp_server_new();          /* port 8554 */
    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    /* The pipeline built for each client. It MUST end in a payloader
     * named pay0; the server creates the SDP from pay0's caps. */
    gst_rtsp_media_factory_set_launch(factory,
        "( videotestsrc is-live=true ! video/x-raw,format=I420,width=640,"
        "height=480,framerate=30/1 ! jpegenc ! rtpjpegpay name=pay0 pt=26 )");
    gst_rtsp_media_factory_set_shared(factory, TRUE); /* 1 pipeline, N clients */

    /* mounts takes ownership of factory */
    gst_rtsp_mount_points_add_factory(mounts, "/test", factory);
    g_object_unref(mounts);

    gst_rtsp_server_attach(server, NULL);   /* listen in the default context */
    g_print("rtsp://127.0.0.1:8554/test\n");
    g_main_loop_run(loop);                  /* serves forever */
    return 0;
}
```

```
 client                         server (gst-rtsp-server)
   │ DESCRIBE rtsp://.../test ──►│ builds the factory pipeline (or reuses shared)
   │◄── SDP (from pay0 caps) ────│
   │ SETUP (ports/transport) ───►│ creates udpsink/rtpbin for this client
   │ PLAY ──────────────────────►│ pipeline → PLAYING
   │◄════════ RTP/RTCP ══════════│
   │ TEARDOWN ──────────────────►│ pipeline → NULL (unless shared & others)
```

Client: `gst-launch-1.0 rtspsrc location=rtsp://IP:8554/test latency=100 !
rtpjpegdepay ! jpegdec ! videoconvert ! autovideosink` (`rtspsrc` is in
plugins-good and *is* installed). `rtspsrc` contains an `rtpbin` with a
jitterbuffer; its `latency` property is that jitterbuffer's latency.

### 3.7 Latency tuning: where the milliseconds hide

```
 capture   encode    pay+send   network   jitterbuf  depay+decode  render
 1 frame   0..N fr   ~0         <1 ms LAN  latency=   0..N frames  sink sync /
 (33 ms)   (B-frames,           ...        50 ms      (threads)    display vsync
           lookahead)
```

| Knob | Where | Effect |
|---|---|---|
| `tune=zerolatency` (x264enc), no B-frames, small GOP | encoder | removes encoder buffering (default x264 can hold >1 s!) |
| `key-int-max` / IDR interval | encoder | faster recovery after loss, more bitrate |
| `latency=` | rtpjitterbuffer / rtspsrc | direct trade: jitter tolerance vs delay |
| `sync=false` | final sink | show on arrival; no waiting for the clock |
| `buffer-size=` | udpsrc / udpsink | kernel socket buffer; too small → burst loss at HD |
| `mtu=` | payloader | keep ≤ path MTU (1400 is safe; IP fragmentation = loss amplifier) |
| `max-threads=1` | avdec_h264 | frame-threaded decoding adds frames of delay |
| `is-live=true` + leaky queue | anywhere | never let a queue grow behind a slow consumer (lesson 13) |
| `qos=true` | sinks | upstream drops late frames instead of accumulating delay |

Measure, don't guess: the `latency` tracer (lesson 12) reports per-element
latency; or overlay `timeoverlay` on the sender and film both screens.

## 4. Complexity table

| Item | Cost | Why |
|---|---|---|
| Raw 64x48 I420 @30 (program 02) | 4.6 KB/frame ≈ 138 KB/s | w·h·1.5 bytes per frame |
| Raw 1080p30 I420 | ≈ 93 MB/s ≈ 750 Mbit/s | why nobody sends raw over 1 GbE |
| MJPEG 640x480 @30, q=80 | ~30–60 KB/frame ≈ 1–2 MB/s | intra-only: each frame independent, robust, simple |
| H.264 720p30 | ~2–4 Mbit/s | inter-frame: 10–50× smaller than MJPEG, but loss propagates until next IDR |
| Packets per frame | ⌈frame size / (MTU − headers)⌉ | 10 KB JPEG at mtu 1400 ≈ 8 packets; lose 1 → lose the frame |
| Header overhead | 40 B per packet (IP+UDP+RTP) + payload hdr | ~3 % at MTU 1400 |
| Copies | payloader: usually 0 (sub-buffers of the frame); depayloader: 1 (concatenate) | RTP pay creates buffer lists pointing into the frame memory |
| Threads | sender: 1 (source); receiver: udpsrc thread + jitterbuffer thread + decoder | the jitterbuffer has its own task to push on its deadlines |
| Added latency | jitterbuffer `latency` + encoder reordering + sink sync | the sum is your glass-to-glass delay |

## 5. C vs C++

The pipelines are the same; only resource handling changes. In C every
`gst_bin_get_by_name()`, `gst_pad_get_current_caps()` and `gst_element_get_bus()`
result must be unreffed by hand (programs 01/02 comment each one). In C++
(lesson 09) wrap them in `std::unique_ptr` with custom deleters, and the
early-return paths of a receiver (timeouts, errors) stop leaking.
gst-rtsp-server is a GObject library, so the same RAII wrappers apply to
`GstRTSPServer*` (`g_object_unref` deleter).

## 6. Where it is used in the real world

* **IP cameras / NVRs**: RTSP + H.264/H.265 over RTP is the de-facto standard
  (ONVIF).
* **Robotics / drones / automotive**: RTP/UDP for low-latency teleoperation;
  on the BeagleBone AI-64 a typical product is `v4l2src → hardware H.264
  encoder → rtph264pay → udpsink` with a few tens of ms end-to-end on a LAN.
* **Broadcast**: SMPTE ST 2110 sends *uncompressed* video as RTP (RFC 4175,
  the `rtpvrawpay` format of program 02) over 10/25/100 GbE.
* **Video calls / WebRTC**: RTP with RTCP feedback (NACK, PLI, congestion
  control); GStreamer's `webrtcbin` (plugins-bad) builds on these bricks.

## 7. Common mistakes and pitfalls

1. **No `caps=` on `udpsrc`** → `not-negotiated`. The packets don't describe
   themselves.
2. **Payload type / encoding-name mismatch** between sender and receiver caps
   (e.g. sender pt=96, receiver caps payload=97) → packets discarded silently.
3. **Expecting EOS at the receiver.** It never comes over UDP; use timeouts.
4. **Receiver started after the sender** with a non-repeating stream: missed
   SPS/PPS → nothing decodes. Use `config-interval=-1` on h264parse /
   rtph264pay, and a short key-int.
5. **Default x264enc settings** for live streaming → seconds of latency.
   Use `tune=zerolatency`.
6. **JPEG with 4:4:4** into rtpjpegpay → "Invalid component", nothing sent
   (seen while writing program 01). Force `format=I420`.
7. **Socket buffer too small** for HD bursts (Linux default ~200 KB): raise
   `buffer-size` and `sysctl net.core.rmem_max`.
8. **MTU larger than the path MTU** (VPNs, VLANs) → IP fragmentation; one
   lost fragment kills the whole packet.
9. **`sync=true` on the receiver sink with mismatched clocks** → frames
   dropped as "too late" or the display freezes; for live view use
   `sync=false`, or proper RTCP sync with rtpbin.
10. Quoting in shell scripts: gst-launch escapes spaces inside **one argv
    element**, so `"$SINK"` with `SINK="fakesink silent=false"` is a syntax
    error. The scripts use `$SINK` unquoted on purpose.

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [01_rtp_jpeg_loopback.c](01_rtp_jpeg_loopback.c) | Sender + receiver pipelines over 127.0.0.1, RTP/JPEG, port 0 auto-allocation, jitterbuffer, frame count + size checked | `make run P=gstreamer/11-network-streaming/01_rtp_jpeg_loopback.c` |
| [02_rtp_raw_video_loopback.c](02_rtp_raw_video_loopback.c) | Uncompressed RTP (RFC 4175); payloader caps read at runtime and given to udpsrc (= SDP) | `make run P=gstreamer/11-network-streaming/02_rtp_raw_video_loopback.c` |
| [01_udp_jpeg_sender.sh](01_udp_jpeg_sender.sh) | RTP/JPEG sender (HOST, PORT, NUM env vars) | `bash gstreamer/11-network-streaming/01_udp_jpeg_sender.sh` |
| [02_udp_jpeg_receiver.sh](02_udp_jpeg_receiver.sh) | Matching receiver, stops after DURATION s | `bash gstreamer/11-network-streaming/02_udp_jpeg_receiver.sh` |
| [03_rtp_vp8_loopback.sh](03_rtp_vp8_loopback.sh) | Compressed inter-frame codec (VP8) over RTP, both ends in one script | `bash gstreamer/11-network-streaming/03_rtp_vp8_loopback.sh` |
| [04_rtp_h264_sender.sh](04_rtp_h264_sender.sh) | Low-latency H.264 sender — **needs plugins-ugly (x264enc) + plugins-bad (h264parse)** | `bash gstreamer/11-network-streaming/04_rtp_h264_sender.sh` |
| [05_rtp_h264_receiver.sh](05_rtp_h264_receiver.sh) | H.264 receiver — **needs plugins-bad + gstreamer1.0-libav** | `bash gstreamer/11-network-streaming/05_rtp_h264_receiver.sh` |
| [06_tcp_mjpeg_loopback.sh](06_tcp_mjpeg_loopback.sh) | MJPEG over TCP with multipart framing | `bash gstreamer/11-network-streaming/06_tcp_mjpeg_loopback.sh` |
| [07_rtsp_server.sh](07_rtsp_server.sh) | RTSP server via gst-rtsp-server's `test-launch` — **needs gst-rtsp-server** | `bash gstreamer/11-network-streaming/07_rtsp_server.sh` |

Programs accept `--display` (run the binary from `build/...`); scripts take
`SINK=fakesink` for headless runs. Try 01/02 scripts across two machines:
sender on the BeagleBone AI-64 (`HOST=<laptop-ip>`), receiver on the laptop.

## 9. Exercises

1. *(easy)* In program 01 set `udpsink sync=false` and `N_FRAMES 300` at
   1280x720. Do frames get lost? Fix it with `udpsrc buffer-size=`.
2. *(easy)* Simulate loss: insert `identity drop-probability=0.05` before
   `udpsink`. How many frames survive with JPEG? Why so many fewer than 95 %?
3. *(medium)* Replace the plain receiver by `rtpbin` and add RTCP (ports P,
   P+1 for RTP/RTCP). Hint: look at the `rtpbin` example in
   `gst-inspect-1.0 rtpbin`.
4. *(medium)* Write a C receiver that uses `udpsrc timeout=` and stops on
   the `GstUDPSrcTimeout` element message instead of the appsink timeout.
5. *(hard)* Measure glass-to-glass latency: put `timeoverlay` on the sender
   and `clockoverlay` on the receiver, show both, take a photo.
6. *(hard)* Build the RTSP sketch above (after installing gst-rtsp-server)
   on the BeagleBone AI-64 with a `v4l2src` camera and connect with VLC.

## 10. Remember forever

Review with the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

```
src ! enc ! PAY ! udpsink   ===UDP===>   udpsrc caps=... ! JITTERBUFFER ! DEPAY ! dec ! sink
RTP header: seq (loss/order), timestamp (90 kHz video), PT (0-95 static, 96+ = per SDP)
Packets carry NO caps -> receiver gets them from SDP / RTSP DESCRIBE / caps= string
EOS never crosses the network: receivers stop on timeout or RTCP BYE
jitterbuffer latency = jitter tolerance = added delay (LAN 20-50 ms)
UDP: drop, constant latency, multicast.  TCP: no loss, latency grows, needs framing
Low latency: zerolatency encoder, short GOP, small jitterbuffer, sink sync=false
H.264: config-interval=-1 so late joiners get SPS/PPS
```

## 11. Self-test questions

1. What do the sequence number, timestamp and payload type of RTP each
   solve?
2. Why must `udpsrc` be given caps, and where do they come from in RTSP?
3. Why does the receiver never see EOS, and how does it know the stream ended?
4. What exactly does `rtpjitterbuffer latency=100` cost and buy?
5. One packet of a 10-packet JPEG frame is lost. What is lost? And for an
   H.264 P-frame?
6. When would you pick TCP over UDP for video?
7. Name four knobs that reduce end-to-end latency.
8. Why is `config-interval=-1` important for a receiver that joins late?
9. What is the role of RTCP, and which element manages RTP+RTCP sessions?
10. On the TDA4VM, which element replaces `x264enc`, and what image do you
    need for it?
